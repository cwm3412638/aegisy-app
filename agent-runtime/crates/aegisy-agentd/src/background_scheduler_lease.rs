//! Durable scheduler-lease contract for background-job ownership.
//!
//! A lease binds observation ownership to an exact durable job generation. It does
//! not grant dispatch, approval, retry, process adoption, or mutation authority.

use crate::background_job::{BackgroundJobRequest, BackgroundJobState};
use crate::background_process_observation::{
    process_observation_required, VerifiedBackgroundProcessRegistration,
};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};

pub const SCHEMA_VERSION: &str = "background-job-scheduler-lease/0.1";
pub const MIN_LEASE_TTL_MS: u64 = 1_000;
pub const MAX_LEASE_TTL_MS: u64 = 5 * 60 * 1_000;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackgroundSchedulerLeaseError {
    pub code: &'static str,
    pub message: &'static str,
}

impl BackgroundSchedulerLeaseError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundSchedulerLeaseStatus {
    Active,
    Released,
    Expired,
}

impl BackgroundSchedulerLeaseStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Active => "active",
            Self::Released => "released",
            Self::Expired => "expired",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundSchedulerLeaseReleaseReason {
    JobTerminal,
    OwnershipYielded,
    RecoveryAbandoned,
    LeaseExpired,
}

impl BackgroundSchedulerLeaseReleaseReason {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::JobTerminal => "job_terminal",
            Self::OwnershipYielded => "ownership_yielded",
            Self::RecoveryAbandoned => "recovery_abandoned",
            Self::LeaseExpired => "lease_expired",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundSchedulerLease {
    pub schema_version: String,
    pub job_id: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub request_identity: String,
    pub state_identity: String,
    pub job_generation: u64,
    pub owner_identity: String,
    pub lease_generation: u64,
    pub status: BackgroundSchedulerLeaseStatus,
    pub acquired_at_ms: u64,
    pub renewed_at_ms: u64,
    pub expires_at_ms: u64,
    pub updated_at_ms: u64,
    pub process_registration_identity: Option<String>,
    pub process_identity: Option<String>,
    pub released_at_ms: Option<u64>,
    pub release_reason: Option<BackgroundSchedulerLeaseReleaseReason>,
    pub dispatch_authority: bool,
    pub automatic_takeover: bool,
    pub lease_identity: String,
}

pub struct BackgroundSchedulerLeaseRebind<'a> {
    pub previous: &'a BackgroundJobState,
    pub next: &'a BackgroundJobState,
    pub owner_identity: &'a str,
    pub process_registration: Option<&'a VerifiedBackgroundProcessRegistration>,
    pub now_ms: u64,
    pub ttl_ms: u64,
}

#[derive(Serialize)]
struct LeaseIdentityBinding<'a> {
    schema_version: &'a str,
    job_id: &'a str,
    session_id: &'a str,
    project_id: &'a str,
    root_id: &'a str,
    request_identity: &'a str,
    state_identity: &'a str,
    job_generation: u64,
    owner_identity: &'a str,
    lease_generation: u64,
    status: BackgroundSchedulerLeaseStatus,
    acquired_at_ms: u64,
    renewed_at_ms: u64,
    expires_at_ms: u64,
    updated_at_ms: u64,
    process_registration_identity: Option<&'a str>,
    process_identity: Option<&'a str>,
    released_at_ms: Option<u64>,
    release_reason: Option<BackgroundSchedulerLeaseReleaseReason>,
    dispatch_authority: bool,
    automatic_takeover: bool,
}

impl BackgroundSchedulerLease {
    pub fn acquire(
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        owner_identity: impl Into<String>,
        now_ms: u64,
        ttl_ms: u64,
    ) -> Result<Self, BackgroundSchedulerLeaseError> {
        validate_job(request, state)?;
        let owner_identity = owner_identity.into();
        if state.status.is_terminal()
            || !valid_owner_identity(&owner_identity)
            || now_ms == 0
            || now_ms < state.updated_at_ms
        {
            return Err(error(
                "background-lease-acquire-invalid",
                "background scheduler lease acquisition is invalid",
            ));
        }
        let expires_at_ms = lease_expiry(now_ms, ttl_ms)?;
        let mut lease = Self {
            schema_version: SCHEMA_VERSION.into(),
            job_id: request.job_id.clone(),
            session_id: request.session_id.clone(),
            project_id: request.project_id.clone(),
            root_id: request.root_id.clone(),
            request_identity: request_identity(request)?,
            state_identity: state_identity(request, state)?,
            job_generation: state.generation,
            owner_identity,
            lease_generation: 1,
            status: BackgroundSchedulerLeaseStatus::Active,
            acquired_at_ms: now_ms,
            renewed_at_ms: now_ms,
            expires_at_ms,
            updated_at_ms: now_ms,
            process_registration_identity: None,
            process_identity: None,
            released_at_ms: None,
            release_reason: None,
            dispatch_authority: false,
            automatic_takeover: false,
            lease_identity: String::new(),
        };
        lease.refresh_identity()?;
        lease.validate(request)?;
        Ok(lease)
    }

    pub fn validate(
        &self,
        request: &BackgroundJobRequest,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        request.validate().map_err(|_| {
            error(
                "background-lease-request-invalid",
                "background scheduler lease request is invalid",
            )
        })?;
        if self.schema_version != SCHEMA_VERSION
            || self.job_id != request.job_id
            || self.session_id != request.session_id
            || self.project_id != request.project_id
            || self.root_id != request.root_id
            || self.request_identity != request_identity(request)?
            || !valid_identity(&self.state_identity, "background-job-state:sha256:")
            || !valid_owner_identity(&self.owner_identity)
            || self.lease_generation == 0
            || self.acquired_at_ms == 0
            || self.renewed_at_ms < self.acquired_at_ms
            || self.expires_at_ms <= self.renewed_at_ms
            || self.expires_at_ms - self.renewed_at_ms < MIN_LEASE_TTL_MS
            || self.expires_at_ms - self.renewed_at_ms > MAX_LEASE_TTL_MS
            || self.updated_at_ms < self.renewed_at_ms
            || self.dispatch_authority
            || self.automatic_takeover
            || self.process_registration_identity.is_some() != self.process_identity.is_some()
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
        {
            return Err(error(
                "background-lease-invalid",
                "background scheduler lease invariant is invalid",
            ));
        }
        match self.status {
            BackgroundSchedulerLeaseStatus::Active
                if self.released_at_ms.is_some() || self.release_reason.is_some() =>
            {
                return Err(error(
                    "background-lease-status-invalid",
                    "active background scheduler lease has terminal metadata",
                ));
            }
            BackgroundSchedulerLeaseStatus::Released
                if self.released_at_ms.is_none()
                    || self.release_reason.is_none_or(|reason| {
                        reason == BackgroundSchedulerLeaseReleaseReason::LeaseExpired
                    }) =>
            {
                return Err(error(
                    "background-lease-status-invalid",
                    "released background scheduler lease metadata is invalid",
                ));
            }
            BackgroundSchedulerLeaseStatus::Expired
                if self.released_at_ms.is_none()
                    || self.release_reason
                        != Some(BackgroundSchedulerLeaseReleaseReason::LeaseExpired) =>
            {
                return Err(error(
                    "background-lease-status-invalid",
                    "expired background scheduler lease metadata is invalid",
                ));
            }
            _ => {}
        }
        if self
            .released_at_ms
            .is_some_and(|released| released < self.renewed_at_ms || released > self.updated_at_ms)
        {
            return Err(error(
                "background-lease-release-time-invalid",
                "background scheduler lease release time is invalid",
            ));
        }
        let expected = lease_identity(self)?;
        if self.lease_identity != expected {
            return Err(error(
                "background-lease-identity-invalid",
                "background scheduler lease identity is invalid",
            ));
        }
        Ok(())
    }

    pub fn validate_for_state(
        &self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        validate_job(request, state)?;
        self.validate(request)?;
        if self.state_identity != state_identity(request, state)?
            || self.job_generation != state.generation
        {
            return Err(error(
                "background-lease-state-stale",
                "background scheduler lease does not bind the current job state",
            ));
        }
        Ok(())
    }

    pub fn renew(
        &mut self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        owner_identity: &str,
        now_ms: u64,
        ttl_ms: u64,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        self.validate_for_state(request, state)?;
        self.require_current_owner(owner_identity, now_ms)?;
        let expires_at_ms = lease_expiry(now_ms, ttl_ms)?;
        self.apply_update(request, |lease| {
            lease.renewed_at_ms = now_ms;
            lease.expires_at_ms = expires_at_ms;
            lease.updated_at_ms = now_ms;
            Ok(())
        })
    }

    pub fn bind_process(
        &mut self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        owner_identity: &str,
        registration: &VerifiedBackgroundProcessRegistration,
        now_ms: u64,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        self.validate_for_state(request, state)?;
        self.require_current_owner(owner_identity, now_ms)?;
        if !process_observation_required(state.status) {
            return Err(error(
                "background-lease-process-state-invalid",
                "background scheduler process binding requires active work",
            ));
        }
        let receipt = registration.registration();
        if receipt.owner_identity != self.owner_identity
            || receipt.job_id != self.job_id
            || receipt.request_identity != self.request_identity
            || receipt.state_identity != self.state_identity
            || receipt.job_generation != self.job_generation
            || receipt.registered_at_ms > now_ms
        {
            return Err(error(
                "background-lease-process-binding-invalid",
                "background scheduler process registration does not bind the lease",
            ));
        }
        match (
            self.process_registration_identity.as_deref(),
            self.process_identity.as_deref(),
        ) {
            (Some(registration_identity), Some(process_identity))
                if registration_identity == receipt.registration_identity
                    && process_identity == receipt.process_identity =>
            {
                return Ok(());
            }
            (Some(_), Some(_)) => {
                return Err(error(
                    "background-lease-process-replacement-denied",
                    "background scheduler lease cannot replace its bound process",
                ));
            }
            _ => {}
        }
        self.apply_update(request, |lease| {
            lease.process_registration_identity = Some(receipt.registration_identity.clone());
            lease.process_identity = Some(receipt.process_identity.clone());
            lease.updated_at_ms = now_ms;
            Ok(())
        })
    }

    pub fn rebind_job_state(
        &mut self,
        request: &BackgroundJobRequest,
        rebind: BackgroundSchedulerLeaseRebind<'_>,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        let BackgroundSchedulerLeaseRebind {
            previous,
            next,
            owner_identity,
            process_registration,
            now_ms,
            ttl_ms,
        } = rebind;
        self.validate_for_state(request, previous)?;
        validate_job(request, next)?;
        self.require_current_owner(owner_identity, now_ms)?;
        if next.generation
            != previous.generation.checked_add(1).ok_or_else(|| {
                error(
                    "background-lease-job-generation-exhausted",
                    "background job generation is exhausted",
                )
            })?
            || next.updated_at_ms < previous.updated_at_ms
        {
            return Err(error(
                "background-lease-job-state-invalid",
                "background scheduler lease state rebind is invalid",
            ));
        }
        let next_state_identity = state_identity(request, next)?;
        let next_expiry = lease_expiry(now_ms, ttl_ms)?;
        let process_binding = process_registration
            .map(|verified| {
                let receipt = verified.registration();
                if receipt.owner_identity != self.owner_identity
                    || receipt.job_id != self.job_id
                    || receipt.request_identity != self.request_identity
                    || receipt.state_identity != next_state_identity
                    || receipt.job_generation != next.generation
                    || receipt.registered_at_ms > now_ms
                {
                    return Err(error(
                        "background-lease-process-rebind-invalid",
                        "background process registration does not bind the next job state",
                    ));
                }
                Ok((
                    receipt.registration_identity.clone(),
                    receipt.process_identity.clone(),
                ))
            })
            .transpose()?;
        self.apply_update(request, |lease| {
            lease.state_identity = next_state_identity;
            lease.job_generation = next.generation;
            lease.renewed_at_ms = now_ms;
            lease.expires_at_ms = next_expiry;
            lease.updated_at_ms = now_ms;
            (lease.process_registration_identity, lease.process_identity) = process_binding
                .map(|(registration, process)| (Some(registration), Some(process)))
                .unwrap_or((None, None));
            Ok(())
        })
    }

    pub fn expire(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        self.validate(request)?;
        if self.status != BackgroundSchedulerLeaseStatus::Active || now_ms < self.expires_at_ms {
            return Err(error(
                "background-lease-not-expired",
                "background scheduler lease is not eligible for expiry",
            ));
        }
        self.apply_update(request, |lease| {
            lease.status = BackgroundSchedulerLeaseStatus::Expired;
            lease.released_at_ms = Some(now_ms);
            lease.release_reason = Some(BackgroundSchedulerLeaseReleaseReason::LeaseExpired);
            lease.updated_at_ms = now_ms;
            Ok(())
        })
    }

    pub fn release(
        &mut self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        owner_identity: &str,
        reason: BackgroundSchedulerLeaseReleaseReason,
        now_ms: u64,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        self.validate_for_state(request, state)?;
        self.require_current_owner(owner_identity, now_ms)?;
        if reason == BackgroundSchedulerLeaseReleaseReason::LeaseExpired
            || (reason == BackgroundSchedulerLeaseReleaseReason::JobTerminal
                && !state.status.is_terminal())
        {
            return Err(error(
                "background-lease-release-reason-invalid",
                "background scheduler lease release reason is invalid",
            ));
        }
        self.apply_update(request, |lease| {
            lease.status = BackgroundSchedulerLeaseStatus::Released;
            lease.released_at_ms = Some(now_ms);
            lease.release_reason = Some(reason);
            lease.updated_at_ms = now_ms;
            Ok(())
        })
    }

    pub fn is_current_for(
        &self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        owner_identity: &str,
        now_ms: u64,
    ) -> bool {
        self.validate_for_state(request, state).is_ok()
            && self.status == BackgroundSchedulerLeaseStatus::Active
            && self.owner_identity == owner_identity
            && now_ms >= self.updated_at_ms
            && now_ms < self.expires_at_ms
    }

    fn require_current_owner(
        &self,
        owner_identity: &str,
        now_ms: u64,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        if self.status != BackgroundSchedulerLeaseStatus::Active
            || self.owner_identity != owner_identity
            || now_ms < self.updated_at_ms
            || now_ms >= self.expires_at_ms
        {
            return Err(error(
                "background-lease-owner-not-current",
                "background scheduler lease owner is not current",
            ));
        }
        Ok(())
    }

    fn apply_update(
        &mut self,
        request: &BackgroundJobRequest,
        update: impl FnOnce(&mut Self) -> Result<(), BackgroundSchedulerLeaseError>,
    ) -> Result<(), BackgroundSchedulerLeaseError> {
        self.validate(request)?;
        let next_generation = self.lease_generation.checked_add(1).ok_or_else(|| {
            error(
                "background-lease-generation-exhausted",
                "background scheduler lease generation is exhausted",
            )
        })?;
        let previous = self.clone();
        if let Err(cause) = update(self) {
            *self = previous;
            return Err(cause);
        }
        self.lease_generation = next_generation;
        if let Err(cause) = self.refresh_identity().and_then(|_| self.validate(request)) {
            *self = previous;
            return Err(cause);
        }
        Ok(())
    }

    fn refresh_identity(&mut self) -> Result<(), BackgroundSchedulerLeaseError> {
        self.lease_identity = lease_identity(self)?;
        Ok(())
    }
}

fn validate_job(
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
) -> Result<(), BackgroundSchedulerLeaseError> {
    request.validate().map_err(|_| {
        error(
            "background-lease-request-invalid",
            "background scheduler lease request is invalid",
        )
    })?;
    state.validate(request).map_err(|_| {
        error(
            "background-lease-state-invalid",
            "background scheduler lease job state is invalid",
        )
    })
}

fn request_identity(
    request: &BackgroundJobRequest,
) -> Result<String, BackgroundSchedulerLeaseError> {
    request.identity().map_err(|_| {
        error(
            "background-lease-request-invalid",
            "background scheduler lease request identity is invalid",
        )
    })
}

fn state_identity(
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
) -> Result<String, BackgroundSchedulerLeaseError> {
    state.identity(request).map_err(|_| {
        error(
            "background-lease-state-invalid",
            "background scheduler lease state identity is invalid",
        )
    })
}

fn lease_expiry(now_ms: u64, ttl_ms: u64) -> Result<u64, BackgroundSchedulerLeaseError> {
    if !(MIN_LEASE_TTL_MS..=MAX_LEASE_TTL_MS).contains(&ttl_ms) {
        return Err(error(
            "background-lease-ttl-invalid",
            "background scheduler lease TTL is invalid",
        ));
    }
    now_ms.checked_add(ttl_ms).ok_or_else(|| {
        error(
            "background-lease-time-exhausted",
            "background scheduler lease time is exhausted",
        )
    })
}

fn lease_identity(
    lease: &BackgroundSchedulerLease,
) -> Result<String, BackgroundSchedulerLeaseError> {
    let bytes = to_vec(&LeaseIdentityBinding {
        schema_version: &lease.schema_version,
        job_id: &lease.job_id,
        session_id: &lease.session_id,
        project_id: &lease.project_id,
        root_id: &lease.root_id,
        request_identity: &lease.request_identity,
        state_identity: &lease.state_identity,
        job_generation: lease.job_generation,
        owner_identity: &lease.owner_identity,
        lease_generation: lease.lease_generation,
        status: lease.status,
        acquired_at_ms: lease.acquired_at_ms,
        renewed_at_ms: lease.renewed_at_ms,
        expires_at_ms: lease.expires_at_ms,
        updated_at_ms: lease.updated_at_ms,
        process_registration_identity: lease.process_registration_identity.as_deref(),
        process_identity: lease.process_identity.as_deref(),
        released_at_ms: lease.released_at_ms,
        release_reason: lease.release_reason,
        dispatch_authority: lease.dispatch_authority,
        automatic_takeover: lease.automatic_takeover,
    })
    .map_err(|_| {
        error(
            "background-lease-identity-failed",
            "background scheduler lease identity could not be created",
        )
    })?;
    Ok(format!(
        "background-job-scheduler-lease:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn valid_owner_identity(value: &str) -> bool {
    valid_identity(value, "scheduler-owner:sha256:")
}

fn valid_identity(value: &str, prefix: &str) -> bool {
    value.strip_prefix(prefix).is_some_and(|hex| {
        hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn error(code: &'static str, message: &'static str) -> BackgroundSchedulerLeaseError {
    BackgroundSchedulerLeaseError::new(code, message)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::background_job::{
        JobRetryPolicy, JobSchedule, JobScheduleKind, REQUEST_SCHEMA_VERSION,
    };
    use crate::background_process_observation::BackgroundProcessRegistry;
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    use std::process::{Command, Stdio};

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn owner(byte: char) -> String {
        identity("scheduler-owner:sha256:", byte)
    }

    fn request() -> BackgroundJobRequest {
        BackgroundJobRequest {
            schema_version: REQUEST_SCHEMA_VERSION.into(),
            job_id: "lease-job".into(),
            session_id: "lease-session".into(),
            project_id: "lease-project".into(),
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
    fn acquisition_and_renewal_bind_exact_owner_state_and_time_without_authority() {
        let request = request();
        let state = BackgroundJobState::new(&request, 100).unwrap();
        let mut lease =
            BackgroundSchedulerLease::acquire(&request, &state, owner('d'), 110, 1_000).unwrap();
        assert!(lease.is_current_for(&request, &state, &owner('d'), 200));
        assert!(!lease.dispatch_authority);
        assert!(!lease.automatic_takeover);
        let initial_identity = lease.lease_identity.clone();
        lease
            .renew(&request, &state, &owner('d'), 500, 2_000)
            .unwrap();
        assert_eq!(lease.lease_generation, 2);
        assert_ne!(lease.lease_identity, initial_identity);
        assert_eq!(
            lease
                .renew(&request, &state, &owner('e'), 600, 1_000)
                .unwrap_err()
                .code,
            "background-lease-owner-not-current"
        );
    }

    #[test]
    fn expiry_and_release_never_enable_takeover_or_infer_job_outcome() {
        let request = request();
        let state = BackgroundJobState::new(&request, 100).unwrap();
        let mut expired =
            BackgroundSchedulerLease::acquire(&request, &state, owner('d'), 110, 1_000).unwrap();
        assert_eq!(
            expired.expire(&request, 1_109).unwrap_err().code,
            "background-lease-not-expired"
        );
        expired.expire(&request, 1_110).unwrap();
        assert_eq!(expired.status, BackgroundSchedulerLeaseStatus::Expired);
        assert!(!expired.automatic_takeover);
        assert!(!expired.dispatch_authority);

        let mut released =
            BackgroundSchedulerLease::acquire(&request, &state, owner('d'), 110, 1_000).unwrap();
        released
            .release(
                &request,
                &state,
                &owner('d'),
                BackgroundSchedulerLeaseReleaseReason::OwnershipYielded,
                200,
            )
            .unwrap();
        assert_eq!(released.status, BackgroundSchedulerLeaseStatus::Released);
        assert!(!released.is_current_for(&request, &state, &owner('d'), 201));
    }

    #[test]
    fn state_drift_and_tampering_fail_closed() {
        let request = request();
        let mut state = BackgroundJobState::new(&request, 100).unwrap();
        let mut lease =
            BackgroundSchedulerLease::acquire(&request, &state, owner('d'), 110, 2_000).unwrap();
        let previous = state.clone();
        state.start(&request, 200).unwrap();
        assert_eq!(
            lease.validate_for_state(&request, &state).unwrap_err().code,
            "background-lease-state-stale"
        );
        lease
            .rebind_job_state(
                &request,
                BackgroundSchedulerLeaseRebind {
                    previous: &previous,
                    next: &state,
                    owner_identity: &owner('d'),
                    process_registration: None,
                    now_ms: 210,
                    ttl_ms: 2_000,
                },
            )
            .unwrap();
        assert!(lease.is_current_for(&request, &state, &owner('d'), 220));
        assert!(lease.process_identity.is_none());

        let mut tampered = lease;
        tampered.job_generation += 1;
        assert_eq!(
            tampered.validate(&request).unwrap_err().code,
            "background-lease-identity-invalid"
        );
    }

    #[cfg(any(target_os = "macos", target_os = "windows"))]
    #[test]
    fn only_verified_runtime_child_registration_can_bind_process_ownership() {
        let request = request();
        let mut state = BackgroundJobState::new(&request, 100).unwrap();
        state.start(&request, 200).unwrap();
        let mut lease =
            BackgroundSchedulerLease::acquire(&request, &state, owner('d'), 210, 2_000).unwrap();
        let mut child = blocking_command()
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let input = child.stdin.take().unwrap();
        let mut registry = BackgroundProcessRegistry::new(owner('d')).unwrap();
        let registration = registry
            .register_spawned_child(&request, &state, child, 210)
            .unwrap();
        lease
            .bind_process(&request, &state, &owner('d'), &registration, 220)
            .unwrap();
        assert!(lease.process_registration_identity.is_some());
        assert!(lease.process_identity.is_some());
        let visible = serde_json::to_string(&lease).unwrap();
        assert!(!visible.contains("\"process_id\":"));
        assert!(!visible.contains("runtime_process_id"));

        let mut replacement_child = blocking_command()
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let replacement_input = replacement_child.stdin.take().unwrap();
        let mut replacement_registry = BackgroundProcessRegistry::new(owner('d')).unwrap();
        let replacement = replacement_registry
            .register_spawned_child(&request, &state, replacement_child, 221)
            .unwrap();
        assert_eq!(
            lease
                .bind_process(&request, &state, &owner('d'), &replacement, 222)
                .unwrap_err()
                .code,
            "background-lease-process-replacement-denied"
        );
        drop(input);
        drop(replacement_input);
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
