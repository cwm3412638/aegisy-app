//! Terminal, metadata-only outcomes for non-Turn mutation reservations.
//!
//! This module validates a terminal acknowledgement against the exact complete
//! source that was reserved. It does not persist an outcome, advertise AAP,
//! dispatch work, or grant permission, approval, mutation, or execution authority.

use crate::approval_ack::{
    ApprovalAcknowledgement, State as ApprovalState, SCHEMA_VERSION as APPROVAL_SCHEMA_VERSION,
};
use crate::background_job::{BackgroundJobState, STATE_SCHEMA_VERSION as JOB_STATE_SCHEMA_VERSION};
use crate::file_write_ack::{
    FileWriteAcknowledgement, State as FileWriteState, SCHEMA_VERSION as FILE_WRITE_SCHEMA_VERSION,
};
use crate::git_mutation_ack::{
    GitMutationAcknowledgement, State as GitMutationState,
    SCHEMA_VERSION as GIT_MUTATION_SCHEMA_VERSION,
};
use crate::mutation_reservation::{MutationReservationKind, MutationReservationSource};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::fmt;

const OUTCOME_IDENTITY_PREFIX: &str = "mutation-reservation-outcome:sha256:";
const MAX_OUTCOME_JSON_BYTES: usize = 16 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MutationReservationOutcomeError {
    pub code: &'static str,
    pub message: &'static str,
}

impl MutationReservationOutcomeError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for MutationReservationOutcomeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

/// Exactly one terminal result from one of the four reservation source kinds.
/// Serialization is transparent so the canonical bytes remain the underlying
/// reviewed acknowledgement or job-state contract.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MutationReservationOutcome {
    Approval(ApprovalAcknowledgement),
    FileWrite(FileWriteAcknowledgement),
    GitMutation(GitMutationAcknowledgement),
    JobSubmission(BackgroundJobState),
}

impl MutationReservationOutcome {
    pub fn from_approval(
        acknowledgement: ApprovalAcknowledgement,
    ) -> Result<Self, MutationReservationOutcomeError> {
        let outcome = Self::Approval(acknowledgement);
        outcome.validate_terminal_shape()?;
        Ok(outcome)
    }

    pub fn from_file_write(
        acknowledgement: FileWriteAcknowledgement,
    ) -> Result<Self, MutationReservationOutcomeError> {
        let outcome = Self::FileWrite(acknowledgement);
        outcome.validate_terminal_shape()?;
        Ok(outcome)
    }

    pub fn from_git_mutation(
        acknowledgement: GitMutationAcknowledgement,
    ) -> Result<Self, MutationReservationOutcomeError> {
        let outcome = Self::GitMutation(acknowledgement);
        outcome.validate_terminal_shape()?;
        Ok(outcome)
    }

    pub fn from_job_submission(
        state: BackgroundJobState,
    ) -> Result<Self, MutationReservationOutcomeError> {
        let outcome = Self::JobSubmission(state);
        outcome.validate_terminal_shape()?;
        Ok(outcome)
    }

    pub fn kind(&self) -> MutationReservationKind {
        match self {
            Self::Approval(_) => MutationReservationKind::Approval,
            Self::FileWrite(_) => MutationReservationKind::FileWrite,
            Self::GitMutation(_) => MutationReservationKind::GitMutation,
            Self::JobSubmission(_) => MutationReservationKind::JobSubmission,
        }
    }

    pub fn schema_version(&self) -> &str {
        match self {
            Self::Approval(value) => &value.schema_version,
            Self::FileWrite(value) => &value.schema_version,
            Self::GitMutation(value) => &value.schema_version,
            Self::JobSubmission(value) => &value.schema_version,
        }
    }

    pub fn state_name(&self) -> &'static str {
        match self {
            Self::Approval(value) => match value.state {
                ApprovalState::Resolved => "resolved",
                ApprovalState::Failed => "failed",
                ApprovalState::Requested => "requested",
                ApprovalState::ReconciliationRequired => "reconciliation-required",
            },
            Self::FileWrite(value) => match value.state {
                FileWriteState::Accepted => "accepted",
                FileWriteState::Committed => "committed",
                FileWriteState::Failed => "failed",
                FileWriteState::ReconciliationRequired => "reconciliation-required",
            },
            Self::GitMutation(value) => match value.state {
                GitMutationState::Accepted => "accepted",
                GitMutationState::Committed => "committed",
                GitMutationState::Failed => "failed",
                GitMutationState::ReconciliationRequired => "reconciliation-required",
            },
            Self::JobSubmission(value) => value.status.as_str(),
        }
    }

    pub fn observed_at_ms(&self) -> u64 {
        match self {
            Self::Approval(value) => value.observed_at_ms,
            Self::FileWrite(value) => value.observed_at_ms,
            Self::GitMutation(value) => value.observed_at_ms,
            Self::JobSubmission(value) => value.updated_at_ms,
        }
    }

    fn validate_terminal_shape(&self) -> Result<(), MutationReservationOutcomeError> {
        match self {
            Self::Approval(value) => {
                value.validate().map_err(|_| {
                    MutationReservationOutcomeError::new(
                        "mutation-reservation-outcome-invalid",
                        "approval outcome is invalid",
                    )
                })?;
                if value.revision != 2
                    || !matches!(value.state, ApprovalState::Resolved | ApprovalState::Failed)
                {
                    return Err(MutationReservationOutcomeError::new(
                        "mutation-reservation-outcome-terminal-invalid",
                        "approval outcome must be a terminal revision-two acknowledgement",
                    ));
                }
            }
            Self::FileWrite(value) => {
                value.validate().map_err(|_| {
                    MutationReservationOutcomeError::new(
                        "mutation-reservation-outcome-invalid",
                        "file-write outcome is invalid",
                    )
                })?;
                if value.revision != 2
                    || !matches!(
                        value.state,
                        FileWriteState::Committed | FileWriteState::Failed
                    )
                {
                    return Err(MutationReservationOutcomeError::new(
                        "mutation-reservation-outcome-terminal-invalid",
                        "file-write outcome must be a terminal revision-two acknowledgement",
                    ));
                }
            }
            Self::GitMutation(value) => {
                value.validate().map_err(|_| {
                    MutationReservationOutcomeError::new(
                        "mutation-reservation-outcome-invalid",
                        "Git mutation outcome is invalid",
                    )
                })?;
                if value.revision != 2
                    || !matches!(
                        value.state,
                        GitMutationState::Committed | GitMutationState::Failed
                    )
                {
                    return Err(MutationReservationOutcomeError::new(
                        "mutation-reservation-outcome-terminal-invalid",
                        "Git mutation outcome must be a terminal revision-two acknowledgement",
                    ));
                }
            }
            Self::JobSubmission(value) => {
                if value.schema_version != JOB_STATE_SCHEMA_VERSION || !value.status.is_terminal() {
                    return Err(MutationReservationOutcomeError::new(
                        "mutation-reservation-outcome-terminal-invalid",
                        "job-submission outcome must be a terminal job state",
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn validate_for_source(
        &self,
        source: &MutationReservationSource,
    ) -> Result<(), MutationReservationOutcomeError> {
        source.validate().map_err(|_| {
            MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-source-invalid",
                "mutation reservation outcome source is invalid",
            )
        })?;
        self.validate_terminal_shape()?;
        let matches = match (self, source) {
            (Self::Approval(outcome), MutationReservationSource::Approval(request)) => {
                outcome.matches_request(request)
            }
            (Self::FileWrite(outcome), MutationReservationSource::FileWrite(request)) => {
                outcome.matches_request(request)
            }
            (Self::GitMutation(outcome), MutationReservationSource::GitMutation(request)) => {
                outcome.matches_request(request)
            }
            (Self::JobSubmission(outcome), MutationReservationSource::JobSubmission(request)) => {
                outcome.validate(request).is_ok()
            }
            _ => false,
        };
        if !matches {
            return Err(MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-source-mismatch",
                "mutation reservation outcome does not match its exact source",
            ));
        }
        Ok(())
    }

    pub fn canonical_bytes_for_source(
        &self,
        source: &MutationReservationSource,
    ) -> Result<Vec<u8>, MutationReservationOutcomeError> {
        self.validate_for_source(source)?;
        let bytes = serde_json::to_vec(self).map_err(|_| {
            MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-serialize-failed",
                "mutation reservation outcome could not be serialized",
            )
        })?;
        if bytes.is_empty() || bytes.len() > MAX_OUTCOME_JSON_BYTES {
            return Err(MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-size-exceeded",
                "mutation reservation outcome exceeds its JSON byte bound",
            ));
        }
        let decoded = Self::from_canonical_bytes_for_source(&bytes, source)?;
        if decoded != *self {
            return Err(MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-round-trip-invalid",
                "mutation reservation outcome changed during canonical serialization",
            ));
        }
        Ok(bytes)
    }

    pub fn from_canonical_bytes_for_source(
        bytes: &[u8],
        source: &MutationReservationSource,
    ) -> Result<Self, MutationReservationOutcomeError> {
        if bytes.is_empty() || bytes.len() > MAX_OUTCOME_JSON_BYTES {
            return Err(MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-size-exceeded",
                "mutation reservation outcome exceeds its JSON byte bound",
            ));
        }
        let outcome: Self = serde_json::from_slice(bytes).map_err(|_| {
            MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-invalid",
                "mutation reservation outcome JSON is invalid",
            )
        })?;
        outcome.validate_for_source(source)?;
        let canonical = serde_json::to_vec(&outcome).map_err(|_| {
            MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-serialize-failed",
                "mutation reservation outcome could not be re-encoded",
            )
        })?;
        if canonical != bytes {
            return Err(MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-canonical-invalid",
                "mutation reservation outcome JSON is not canonical",
            ));
        }
        Ok(outcome)
    }

    pub fn canonical_sha256_for_source(
        &self,
        source: &MutationReservationSource,
    ) -> Result<String, MutationReservationOutcomeError> {
        Ok(format!(
            "{:x}",
            Sha256::digest(self.canonical_bytes_for_source(source)?)
        ))
    }

    pub fn outcome_identity(
        &self,
        source: &MutationReservationSource,
    ) -> Result<String, MutationReservationOutcomeError> {
        let source_identity = source.source_identity().map_err(|_| {
            MutationReservationOutcomeError::new(
                "mutation-reservation-outcome-source-invalid",
                "mutation reservation outcome source identity is invalid",
            )
        })?;
        let canonical = self.canonical_bytes_for_source(source)?;
        let kind = kind_name(self.kind());
        let mut digest = Sha256::new();
        digest.update(b"aegisy-mutation-reservation-outcome/0.1\0");
        for value in [
            source_identity.as_bytes(),
            kind.as_bytes(),
            canonical.as_slice(),
        ] {
            digest.update((value.len() as u64).to_be_bytes());
            digest.update(value);
        }
        Ok(format!("{OUTCOME_IDENTITY_PREFIX}{:x}", digest.finalize()))
    }
}

fn kind_name(kind: MutationReservationKind) -> &'static str {
    match kind {
        MutationReservationKind::Approval => "approval",
        MutationReservationKind::FileWrite => "file-write",
        MutationReservationKind::GitMutation => "git-mutation",
        MutationReservationKind::JobSubmission => "job-submission",
    }
}

impl Serialize for MutationReservationOutcome {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        match self {
            Self::Approval(value) => value.serialize(serializer),
            Self::FileWrite(value) => value.serialize(serializer),
            Self::GitMutation(value) => value.serialize(serializer),
            Self::JobSubmission(value) => value.serialize(serializer),
        }
    }
}

impl<'de> Deserialize<'de> for MutationReservationOutcome {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let value = Value::deserialize(deserializer)?;
        let schema_version = value
            .get("schema_version")
            .and_then(Value::as_str)
            .ok_or_else(|| serde::de::Error::custom("outcome schema version is missing"))?;
        let outcome = match schema_version {
            APPROVAL_SCHEMA_VERSION => {
                Self::Approval(serde_json::from_value(value).map_err(serde::de::Error::custom)?)
            }
            FILE_WRITE_SCHEMA_VERSION => {
                Self::FileWrite(serde_json::from_value(value).map_err(serde::de::Error::custom)?)
            }
            GIT_MUTATION_SCHEMA_VERSION => {
                Self::GitMutation(serde_json::from_value(value).map_err(serde::de::Error::custom)?)
            }
            JOB_STATE_SCHEMA_VERSION => {
                let object = value
                    .as_object()
                    .ok_or_else(|| serde::de::Error::custom("job outcome must be an object"))?;
                const JOB_KEYS: [&str; 10] = [
                    "schema_version",
                    "request_identity",
                    "status",
                    "cancellation",
                    "generation",
                    "created_at_ms",
                    "updated_at_ms",
                    "next_eligible_at_ms",
                    "approval_identity",
                    "result_reference",
                ];
                if object.len() != JOB_KEYS.len() + 1
                    || !object
                        .keys()
                        .all(|key| JOB_KEYS.contains(&key.as_str()) || key == "attempts")
                {
                    return Err(serde::de::Error::custom(
                        "job outcome contains an unknown or missing field",
                    ));
                }
                Self::JobSubmission(
                    serde_json::from_value(value).map_err(serde::de::Error::custom)?,
                )
            }
            _ => {
                return Err(serde::de::Error::custom(
                    "outcome schema version is unsupported",
                ))
            }
        };
        outcome
            .validate_terminal_shape()
            .map_err(serde::de::Error::custom)?;
        Ok(outcome)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::approval_ack::{ApprovalRequest, Resolution, Scope};
    use crate::background_job::{
        BackgroundJobRequest, JobRetryPolicy, JobSchedule, JobScheduleKind,
    };
    use crate::file_write_ack::FileWriteRequest;
    use crate::git_mutation_ack::{GitMutationRequest, Kind as GitMutationKind};

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn approval_pair() -> (MutationReservationSource, MutationReservationOutcome) {
        let request = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "approval-key",
            identity("request:sha256:", 'a'),
            Scope::FileChange,
        )
        .unwrap();
        let outcome = request
            .acknowledgement(
                ApprovalState::Resolved,
                Resolution::Denied,
                2,
                Some(identity("approval-observation:sha256:", 'b')),
                11,
            )
            .unwrap();
        (
            MutationReservationSource::from_approval(request).unwrap(),
            MutationReservationOutcome::from_approval(outcome).unwrap(),
        )
    }

    fn file_write_pair() -> (MutationReservationSource, MutationReservationOutcome) {
        let request = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "file-key",
            identity("request:sha256:", 'c'),
            identity("workspace-edit:sha256:", 'd'),
            2,
            64,
        )
        .unwrap();
        let outcome = request
            .acknowledgement(
                FileWriteState::Committed,
                2,
                Some(identity("file-write-observation:sha256:", 'e')),
                12,
            )
            .unwrap();
        (
            MutationReservationSource::from_file_write(request).unwrap(),
            MutationReservationOutcome::from_file_write(outcome).unwrap(),
        )
    }

    fn git_pair() -> (MutationReservationSource, MutationReservationOutcome) {
        let request = GitMutationRequest::new(
            "session-1",
            "project-1",
            "root-1",
            GitMutationKind::Commit,
            "git-key",
            identity("request:sha256:", 'f'),
            identity("git-plan:sha256:", 'a'),
        )
        .unwrap();
        let outcome = request
            .acknowledgement(
                GitMutationState::Committed,
                2,
                Some(identity("git-mutation-observation:sha256:", 'b')),
                13,
            )
            .unwrap();
        (
            MutationReservationSource::from_git_mutation(request).unwrap(),
            MutationReservationOutcome::from_git_mutation(outcome).unwrap(),
        )
    }

    fn job_request(job_id: &str) -> BackgroundJobRequest {
        BackgroundJobRequest {
            schema_version: crate::background_job::REQUEST_SCHEMA_VERSION.into(),
            job_id: job_id.into(),
            session_id: "session-1".into(),
            project_id: "project-1".into(),
            root_id: "root-1".into(),
            execution_plan_identity: identity("unified-execution-plan:sha256:", 'c'),
            idempotency_identity: identity("idempotency:sha256:", 'd'),
            child_task_identity: None,
            schedule: JobSchedule {
                kind: JobScheduleKind::Manual,
                scheduled_for_ms: None,
            },
            retry: JobRetryPolicy {
                max_attempts: 2,
                backoff_ms: 10,
                safe_retry_boundary_identity: Some(identity("retry-boundary:sha256:", 'e')),
            },
            created_at_ms: 10,
        }
    }

    fn job_pair() -> (MutationReservationSource, MutationReservationOutcome) {
        let request = job_request("job-1");
        let mut state = BackgroundJobState::new(&request, 10).unwrap();
        state.start(&request, 11).unwrap();
        state
            .complete(
                &request,
                &identity("artifact:sha256:", 'f'),
                &identity("job-evidence:sha256:", 'a'),
                14,
            )
            .unwrap();
        (
            MutationReservationSource::from_job_submission(request).unwrap(),
            MutationReservationOutcome::from_job_submission(state).unwrap(),
        )
    }

    #[test]
    fn all_terminal_outcomes_round_trip_canonically_against_exact_sources() {
        for (source, outcome) in [approval_pair(), file_write_pair(), git_pair(), job_pair()] {
            outcome.validate_for_source(&source).unwrap();
            let bytes = outcome.canonical_bytes_for_source(&source).unwrap();
            assert_eq!(
                MutationReservationOutcome::from_canonical_bytes_for_source(&bytes, &source)
                    .unwrap(),
                outcome
            );
            assert_eq!(
                outcome.canonical_sha256_for_source(&source).unwrap().len(),
                64
            );
            assert!(outcome
                .outcome_identity(&source)
                .unwrap()
                .starts_with(OUTCOME_IDENTITY_PREFIX));
        }
    }

    #[test]
    fn non_terminal_or_uncertain_values_are_not_outcomes() {
        let (file_source, _) = file_write_pair();
        let MutationReservationSource::FileWrite(file_request) = file_source else {
            unreachable!();
        };
        let accepted = file_request
            .acknowledgement(FileWriteState::Accepted, 1, None, 10)
            .unwrap();
        assert_eq!(
            MutationReservationOutcome::from_file_write(accepted)
                .unwrap_err()
                .code,
            "mutation-reservation-outcome-terminal-invalid"
        );

        let job_request = job_request("job-1");
        let queued = BackgroundJobState::new(&job_request, 10).unwrap();
        assert_eq!(
            MutationReservationOutcome::from_job_submission(queued)
                .unwrap_err()
                .code,
            "mutation-reservation-outcome-terminal-invalid"
        );
    }

    #[test]
    fn acknowledgement_outcome_must_be_exact_revision_two() {
        let (source, _) = git_pair();
        let MutationReservationSource::GitMutation(request) = source else {
            unreachable!();
        };
        let revision_three = request
            .acknowledgement(
                GitMutationState::Failed,
                3,
                Some(identity("git-mutation-observation:sha256:", 'f')),
                20,
            )
            .unwrap();
        assert_eq!(
            MutationReservationOutcome::from_git_mutation(revision_three)
                .unwrap_err()
                .code,
            "mutation-reservation-outcome-terminal-invalid"
        );
    }

    #[test]
    fn source_kind_and_binding_drift_fail_closed() {
        let (approval_source, approval_outcome) = approval_pair();
        let (file_source, file_outcome) = file_write_pair();
        assert_eq!(
            approval_outcome
                .validate_for_source(&file_source)
                .unwrap_err()
                .code,
            "mutation-reservation-outcome-source-mismatch"
        );

        let MutationReservationSource::FileWrite(original_request) = file_source else {
            unreachable!();
        };
        let drifted_request = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "different-key",
            original_request.request_fingerprint.clone(),
            original_request.edit_identity.clone(),
            original_request.changed_files,
            original_request.requested_bytes,
        )
        .unwrap();
        let drifted_source = MutationReservationSource::from_file_write(drifted_request).unwrap();
        assert_eq!(
            file_outcome
                .validate_for_source(&drifted_source)
                .unwrap_err()
                .code,
            "mutation-reservation-outcome-source-mismatch"
        );
        approval_outcome
            .validate_for_source(&approval_source)
            .unwrap();
    }

    #[test]
    fn job_state_must_match_the_exact_reserved_request() {
        let (source, outcome) = job_pair();
        outcome.validate_for_source(&source).unwrap();
        let drifted = MutationReservationSource::from_job_submission(job_request("job-2")).unwrap();
        assert_eq!(
            outcome.validate_for_source(&drifted).unwrap_err().code,
            "mutation-reservation-outcome-source-mismatch"
        );
    }

    #[test]
    fn noncanonical_unknown_and_authority_fields_are_rejected() {
        let (source, outcome) = approval_pair();
        let bytes = outcome.canonical_bytes_for_source(&source).unwrap();
        let mut padded = b" ".to_vec();
        padded.extend_from_slice(&bytes);
        assert_eq!(
            MutationReservationOutcome::from_canonical_bytes_for_source(&padded, &source)
                .unwrap_err()
                .code,
            "mutation-reservation-outcome-canonical-invalid"
        );

        let mut unknown: Value = serde_json::from_slice(&bytes).unwrap();
        unknown["command"] = serde_json::json!("must-not-run");
        assert!(serde_json::from_value::<MutationReservationOutcome>(unknown).is_err());

        let mut authority: Value = serde_json::from_slice(&bytes).unwrap();
        authority["mutation_authority"] = serde_json::json!(true);
        assert!(serde_json::from_value::<MutationReservationOutcome>(authority).is_err());
    }

    #[test]
    fn job_outcome_rejects_unknown_fields_despite_legacy_state_serde() {
        let (source, outcome) = job_pair();
        let mut value = serde_json::to_value(&outcome).unwrap();
        value["dispatch_authority"] = serde_json::json!(false);
        assert!(serde_json::from_value::<MutationReservationOutcome>(value).is_err());
        outcome.validate_for_source(&source).unwrap();
    }
}
