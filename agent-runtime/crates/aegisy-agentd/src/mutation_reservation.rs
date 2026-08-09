//! Content-free reservation drafts for non-Turn mutation-shaped requests.
//!
//! Schema-v20's durable mutation ledger is intentionally Turn-specific: its
//! only kind is `turn-start`, and every observed transition binds Turn Timeline
//! anchors. These drafts preserve the exact existing approval, file-write, Git,
//! and background-job request bindings while explicitly declaring that they
//! are not compatible with that Store schema. A schema-v21 Store wrapper may
//! persist a validated draft as non-authorizing reservation evidence; the
//! draft's own fixed-false persistence field is not that evidence. Drafts do
//! not advertise AAP, dispatch work, or grant any authority.

use crate::approval_ack::{ApprovalRequest, Scope as ApprovalScope};
use crate::background_job::BackgroundJobRequest;
use crate::file_write_ack::FileWriteRequest;
use crate::git_mutation_ack::{GitMutationRequest, Kind as GitMutationKind};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "mutation-reservation-draft/0.1";
const RESERVATION_IDENTITY_PREFIX: &str = "mutation-reservation-draft:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MutationReservationError {
    pub code: &'static str,
    pub message: &'static str,
}

impl MutationReservationError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for MutationReservationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum MutationReservationKind {
    Approval,
    FileWrite,
    GitMutation,
    JobSubmission,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RetryDisposition {
    Replay,
    Conflict,
    Unrelated,
}

impl MutationReservationKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::Approval => "approval",
            Self::FileWrite => "file-write",
            Self::GitMutation => "git-mutation",
            Self::JobSubmission => "job-submission",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MutationReservationDraft {
    pub schema_version: String,
    pub reservation_identity: String,
    pub kind: MutationReservationKind,
    pub source_schema_version: String,
    pub source_subkind: String,
    pub source_operation_identity: String,
    pub source_binding_identity: String,
    pub session_id: String,
    pub project_id: Option<String>,
    pub root_id: Option<String>,
    pub turn_id: Option<String>,
    pub idempotency_key: String,
    /// Lowercase raw SHA-256, matching the current durable-ledger fingerprint
    /// representation without claiming that the row may enter schema v20.
    pub request_fingerprint: String,
    pub v20_store_compatible: bool,
    pub turn_timeline_anchor_compatible: bool,
    pub reservation_persisted: bool,
    pub dispatch_authority: bool,
    pub mutation_authority: bool,
    pub approval_authority: bool,
    pub execution_authority: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct DraftWire {
    schema_version: String,
    reservation_identity: String,
    kind: MutationReservationKind,
    source_schema_version: String,
    source_subkind: String,
    source_operation_identity: String,
    source_binding_identity: String,
    session_id: String,
    project_id: Option<String>,
    root_id: Option<String>,
    turn_id: Option<String>,
    idempotency_key: String,
    request_fingerprint: String,
    v20_store_compatible: bool,
    turn_timeline_anchor_compatible: bool,
    reservation_persisted: bool,
    dispatch_authority: bool,
    mutation_authority: bool,
    approval_authority: bool,
    execution_authority: bool,
}

fn jwt_shaped(value: &str) -> bool {
    let segments = value.split('.').collect::<Vec<_>>();
    segments.len() == 3
        && segments.iter().all(|segment| {
            segment.len() >= 8
                && segment
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
        })
}

fn secret_shaped(value: &str) -> bool {
    if jwt_shaped(value) {
        return true;
    }
    let lowercase = value.to_ascii_lowercase();
    [
        "api_key",
        "api-key",
        "apikey",
        "access_token",
        "access-token",
        "authorization",
        "bearer",
        "client_secret",
        "client-secret",
        "cookie",
        "credential",
        "password",
        "private_key",
        "private-key",
        "refresh_token",
        "refresh-token",
        "secret",
    ]
    .iter()
    .any(|marker| lowercase.contains(marker))
        || value
            .split(|character: char| {
                !character.is_ascii_alphanumeric()
                    && character != '_'
                    && character != '-'
                    && character != '.'
            })
            .any(|token| {
                (token.starts_with("sk-") && token.len() >= 20)
                    || (token.starts_with("ghp_") && token.len() >= 20)
                    || (token.starts_with("github_pat_") && token.len() >= 24)
                    || jwt_shaped(token)
            })
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_IDENTIFIER_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
        && !secret_shaped(value)
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn valid_sha256_identity(value: &str, prefix: &str) -> bool {
    value.len() == prefix.len() + 64
        && value.starts_with(prefix)
        && valid_sha256(&value[prefix.len()..])
}

fn normalized_request_fingerprint(value: &str) -> Result<String, MutationReservationError> {
    let normalized = value.strip_prefix("request:sha256:").unwrap_or(value);
    if !valid_sha256(normalized) {
        return Err(MutationReservationError::new(
            "mutation-reservation-fingerprint-invalid",
            "mutation reservation request fingerprint is invalid",
        ));
    }
    Ok(normalized.into())
}

fn approval_subkind(scope: ApprovalScope) -> &'static str {
    match scope {
        ApprovalScope::CommandExecution => "command-execution",
        ApprovalScope::FileChange => "file-change",
        ApprovalScope::Permissions => "permissions",
    }
}

fn git_subkind(kind: GitMutationKind) -> &'static str {
    match kind {
        GitMutationKind::BranchCreate => "branch-create",
        GitMutationKind::BranchSwitch => "branch-switch",
        GitMutationKind::BranchRename => "branch-rename",
        GitMutationKind::Stage => "stage",
        GitMutationKind::Commit => "commit",
        GitMutationKind::WorktreeCreate => "worktree-create",
        GitMutationKind::WorktreeRemove => "worktree-remove",
    }
}

impl MutationReservationDraft {
    pub fn from_approval(request: &ApprovalRequest) -> Result<Self, MutationReservationError> {
        request.validate().map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-invalid",
                "approval reservation source is invalid",
            )
        })?;
        Self::build(
            MutationReservationKind::Approval,
            crate::approval_ack::SCHEMA_VERSION,
            approval_subkind(request.scope),
            &request.operation_identity,
            &request.requirement_identity,
            &request.session_id,
            None,
            None,
            Some(&request.turn_id),
            &request.idempotency_key,
            &request.request_fingerprint,
        )
    }

    pub fn from_file_write(request: &FileWriteRequest) -> Result<Self, MutationReservationError> {
        request.validate().map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-invalid",
                "file-write reservation source is invalid",
            )
        })?;
        Self::build(
            MutationReservationKind::FileWrite,
            crate::file_write_ack::SCHEMA_VERSION,
            "workspace-edit",
            &request.operation_identity,
            &request.edit_identity,
            &request.session_id,
            Some(&request.project_id),
            Some(&request.root_id),
            None,
            &request.idempotency_key,
            &request.request_fingerprint,
        )
    }

    pub fn from_git_mutation(
        request: &GitMutationRequest,
    ) -> Result<Self, MutationReservationError> {
        request.validate().map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-invalid",
                "Git mutation reservation source is invalid",
            )
        })?;
        Self::build(
            MutationReservationKind::GitMutation,
            crate::git_mutation_ack::SCHEMA_VERSION,
            git_subkind(request.kind),
            &request.operation_identity,
            &request.plan_identity,
            &request.session_id,
            Some(&request.project_id),
            Some(&request.root_id),
            None,
            &request.idempotency_key,
            &request.request_fingerprint,
        )
    }

    pub fn from_job_submission(
        request: &BackgroundJobRequest,
    ) -> Result<Self, MutationReservationError> {
        request.validate().map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-invalid",
                "job-submission reservation source is invalid",
            )
        })?;
        let operation_identity = request.identity().map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-invalid",
                "job-submission reservation identity is unavailable",
            )
        })?;
        let request_fingerprint = operation_identity
            .strip_prefix("background-job:sha256:")
            .ok_or_else(|| {
                MutationReservationError::new(
                    "mutation-reservation-source-invalid",
                    "job-submission identity is invalid",
                )
            })?;
        Self::build(
            MutationReservationKind::JobSubmission,
            crate::background_job::REQUEST_SCHEMA_VERSION,
            "background-job",
            &operation_identity,
            &request.execution_plan_identity,
            &request.session_id,
            Some(&request.project_id),
            Some(&request.root_id),
            None,
            &request.idempotency_identity,
            request_fingerprint,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn build(
        kind: MutationReservationKind,
        source_schema_version: &str,
        source_subkind: &str,
        source_operation_identity: &str,
        source_binding_identity: &str,
        session_id: &str,
        project_id: Option<&str>,
        root_id: Option<&str>,
        turn_id: Option<&str>,
        idempotency_key: &str,
        request_fingerprint: &str,
    ) -> Result<Self, MutationReservationError> {
        let mut draft = Self {
            schema_version: SCHEMA_VERSION.into(),
            reservation_identity: String::new(),
            kind,
            source_schema_version: source_schema_version.into(),
            source_subkind: source_subkind.into(),
            source_operation_identity: source_operation_identity.into(),
            source_binding_identity: source_binding_identity.into(),
            session_id: session_id.into(),
            project_id: project_id.map(str::to_owned),
            root_id: root_id.map(str::to_owned),
            turn_id: turn_id.map(str::to_owned),
            idempotency_key: idempotency_key.into(),
            request_fingerprint: normalized_request_fingerprint(request_fingerprint)?,
            v20_store_compatible: false,
            turn_timeline_anchor_compatible: false,
            reservation_persisted: false,
            dispatch_authority: false,
            mutation_authority: false,
            approval_authority: false,
            execution_authority: false,
        };
        draft.reservation_identity = draft.derived_identity();
        draft.validate()?;
        Ok(draft)
    }

    fn derived_identity(&self) -> String {
        let mut digest = Sha256::new();
        digest.update(b"aegisy-mutation-reservation-draft/0.1\0");
        for value in [
            self.kind.as_str(),
            &self.source_schema_version,
            &self.source_subkind,
            &self.source_operation_identity,
            &self.source_binding_identity,
            &self.session_id,
            self.project_id.as_deref().unwrap_or(""),
            self.root_id.as_deref().unwrap_or(""),
            self.turn_id.as_deref().unwrap_or(""),
            &self.idempotency_key,
            &self.request_fingerprint,
        ] {
            digest.update((value.len() as u64).to_be_bytes());
            digest.update(value.as_bytes());
        }
        format!("{RESERVATION_IDENTITY_PREFIX}{:x}", digest.finalize())
    }

    pub fn validate(&self) -> Result<(), MutationReservationError> {
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.reservation_identity, RESERVATION_IDENTITY_PREFIX)
            || self.reservation_identity != self.derived_identity()
            || !valid_identifier(&self.source_schema_version)
            || !valid_identifier(&self.source_subkind)
            || !valid_identifier(&self.session_id)
            || !valid_identifier(&self.idempotency_key)
            || !valid_sha256(&self.request_fingerprint)
        {
            return Err(MutationReservationError::new(
                "mutation-reservation-invalid",
                "mutation reservation metadata or identity is invalid",
            ));
        }
        for identifier in [
            self.project_id.as_deref(),
            self.root_id.as_deref(),
            self.turn_id.as_deref(),
        ]
        .into_iter()
        .flatten()
        {
            if !valid_identifier(identifier) {
                return Err(MutationReservationError::new(
                    "mutation-reservation-binding-invalid",
                    "mutation reservation scope binding is invalid",
                ));
            }
        }
        let (operation_prefix, binding_prefix, expected_schema, valid_subkind, valid_scope) =
            match self.kind {
                MutationReservationKind::Approval => (
                    "approval-operation:sha256:",
                    "approval-requirement:sha256:",
                    crate::approval_ack::SCHEMA_VERSION,
                    matches!(
                        self.source_subkind.as_str(),
                        "command-execution" | "file-change" | "permissions"
                    ),
                    self.project_id.is_none() && self.root_id.is_none() && self.turn_id.is_some(),
                ),
                MutationReservationKind::FileWrite => (
                    "file-write-operation:sha256:",
                    "workspace-edit:sha256:",
                    crate::file_write_ack::SCHEMA_VERSION,
                    self.source_subkind == "workspace-edit",
                    self.project_id.is_some() && self.root_id.is_some() && self.turn_id.is_none(),
                ),
                MutationReservationKind::GitMutation => (
                    "git-mutation-operation:sha256:",
                    "git-plan:sha256:",
                    crate::git_mutation_ack::SCHEMA_VERSION,
                    matches!(
                        self.source_subkind.as_str(),
                        "branch-create"
                            | "branch-switch"
                            | "branch-rename"
                            | "stage"
                            | "commit"
                            | "worktree-create"
                            | "worktree-remove"
                    ),
                    self.project_id.is_some() && self.root_id.is_some() && self.turn_id.is_none(),
                ),
                MutationReservationKind::JobSubmission => (
                    "background-job:sha256:",
                    "unified-execution-plan:sha256:",
                    crate::background_job::REQUEST_SCHEMA_VERSION,
                    self.source_subkind == "background-job",
                    self.project_id.is_some() && self.root_id.is_some() && self.turn_id.is_none(),
                ),
            };
        if self.source_schema_version != expected_schema
            || !valid_sha256_identity(&self.source_operation_identity, operation_prefix)
            || !valid_sha256_identity(&self.source_binding_identity, binding_prefix)
            || !valid_subkind
            || !valid_scope
            || (self.kind == MutationReservationKind::JobSubmission
                && self
                    .source_operation_identity
                    .strip_prefix(operation_prefix)
                    != Some(self.request_fingerprint.as_str()))
        {
            return Err(MutationReservationError::new(
                "mutation-reservation-source-binding-invalid",
                "mutation reservation source binding is invalid",
            ));
        }
        if self.v20_store_compatible
            || self.turn_timeline_anchor_compatible
            || self.reservation_persisted
            || self.dispatch_authority
            || self.mutation_authority
            || self.approval_authority
            || self.execution_authority
        {
            return Err(MutationReservationError::new(
                "mutation-reservation-authority-invalid",
                "mutation reservation draft cannot claim persistence, compatibility, or authority",
            ));
        }
        Ok(())
    }

    /// Classifies a retry using the future ledger's intended uniqueness key.
    /// A matching Session/kind/idempotency tuple is replayable only when the
    /// complete validated reservation is byte-for-byte equivalent.
    pub fn retry_disposition(&self, existing: &Self) -> RetryDisposition {
        if self.session_id != existing.session_id
            || self.kind != existing.kind
            || self.idempotency_key != existing.idempotency_key
        {
            return RetryDisposition::Unrelated;
        }
        if self.validate().is_ok() && existing.validate().is_ok() && self == existing {
            RetryDisposition::Replay
        } else {
            RetryDisposition::Conflict
        }
    }
}

impl<'de> Deserialize<'de> for MutationReservationDraft {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = DraftWire::deserialize(deserializer)?;
        let draft = Self {
            schema_version: wire.schema_version,
            reservation_identity: wire.reservation_identity,
            kind: wire.kind,
            source_schema_version: wire.source_schema_version,
            source_subkind: wire.source_subkind,
            source_operation_identity: wire.source_operation_identity,
            source_binding_identity: wire.source_binding_identity,
            session_id: wire.session_id,
            project_id: wire.project_id,
            root_id: wire.root_id,
            turn_id: wire.turn_id,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            v20_store_compatible: wire.v20_store_compatible,
            turn_timeline_anchor_compatible: wire.turn_timeline_anchor_compatible,
            reservation_persisted: wire.reservation_persisted,
            dispatch_authority: wire.dispatch_authority,
            mutation_authority: wire.mutation_authority,
            approval_authority: wire.approval_authority,
            execution_authority: wire.execution_authority,
        };
        draft.validate().map_err(serde::de::Error::custom)?;
        Ok(draft)
    }
}

impl Serialize for MutationReservationDraft {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct DraftRef<'a> {
            schema_version: &'a str,
            reservation_identity: &'a str,
            kind: MutationReservationKind,
            source_schema_version: &'a str,
            source_subkind: &'a str,
            source_operation_identity: &'a str,
            source_binding_identity: &'a str,
            session_id: &'a str,
            project_id: Option<&'a str>,
            root_id: Option<&'a str>,
            turn_id: Option<&'a str>,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            v20_store_compatible: bool,
            turn_timeline_anchor_compatible: bool,
            reservation_persisted: bool,
            dispatch_authority: bool,
            mutation_authority: bool,
            approval_authority: bool,
            execution_authority: bool,
        }
        DraftRef {
            schema_version: &self.schema_version,
            reservation_identity: &self.reservation_identity,
            kind: self.kind,
            source_schema_version: &self.source_schema_version,
            source_subkind: &self.source_subkind,
            source_operation_identity: &self.source_operation_identity,
            source_binding_identity: &self.source_binding_identity,
            session_id: &self.session_id,
            project_id: self.project_id.as_deref(),
            root_id: self.root_id.as_deref(),
            turn_id: self.turn_id.as_deref(),
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            v20_store_compatible: false,
            turn_timeline_anchor_compatible: false,
            reservation_persisted: false,
            dispatch_authority: false,
            mutation_authority: false,
            approval_authority: false,
            execution_authority: false,
        }
        .serialize(serializer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::approval_ack::Scope;
    use crate::background_job::{JobRetryPolicy, JobSchedule, JobScheduleKind};
    use serde_json::json;

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn fingerprint(byte: char) -> String {
        identity("request:sha256:", byte)
    }

    #[test]
    fn existing_contracts_produce_content_free_v20_incompatible_drafts() {
        let approval = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "approval-retry-1",
            fingerprint('a'),
            Scope::FileChange,
        )
        .unwrap();
        let file_write = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "file-retry-1",
            fingerprint('b'),
            identity("workspace-edit:sha256:", 'c'),
            2,
            128,
        )
        .unwrap();
        let git = GitMutationRequest::new(
            "session-1",
            "project-1",
            "root-1",
            GitMutationKind::Commit,
            "git-retry-1",
            fingerprint('d'),
            identity("git-plan:sha256:", 'e'),
        )
        .unwrap();
        let job = BackgroundJobRequest {
            schema_version: crate::background_job::REQUEST_SCHEMA_VERSION.into(),
            job_id: "job-1".into(),
            session_id: "session-1".into(),
            project_id: "project-1".into(),
            root_id: "root-1".into(),
            execution_plan_identity: identity("unified-execution-plan:sha256:", 'f'),
            idempotency_identity: identity("idempotency:sha256:", '1'),
            child_task_identity: None,
            schedule: JobSchedule {
                kind: JobScheduleKind::Manual,
                scheduled_for_ms: None,
            },
            retry: JobRetryPolicy {
                max_attempts: 1,
                backoff_ms: 0,
                safe_retry_boundary_identity: None,
            },
            created_at_ms: 10,
        };

        let drafts = [
            MutationReservationDraft::from_approval(&approval).unwrap(),
            MutationReservationDraft::from_file_write(&file_write).unwrap(),
            MutationReservationDraft::from_git_mutation(&git).unwrap(),
            MutationReservationDraft::from_job_submission(&job).unwrap(),
        ];
        assert_eq!(
            drafts.iter().map(|draft| draft.kind).collect::<Vec<_>>(),
            vec![
                MutationReservationKind::Approval,
                MutationReservationKind::FileWrite,
                MutationReservationKind::GitMutation,
                MutationReservationKind::JobSubmission,
            ]
        );
        for draft in drafts {
            draft.validate().unwrap();
            assert!(!draft.v20_store_compatible);
            assert!(!draft.turn_timeline_anchor_compatible);
            assert!(!draft.reservation_persisted);
            assert!(!draft.dispatch_authority);
            assert!(!draft.mutation_authority);
            assert!(!draft.approval_authority);
            assert!(!draft.execution_authority);
            let encoded = serde_json::to_value(&draft).unwrap();
            for forbidden in [
                "prompt",
                "context",
                "body",
                "path",
                "command",
                "credential",
                "accepted_anchor",
                "terminal_anchor",
            ] {
                assert!(encoded.get(forbidden).is_none());
            }
        }
    }

    #[test]
    fn normalized_fingerprint_and_identity_are_stable_for_exact_retries() {
        let request = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "file-retry-1",
            fingerprint('a'),
            identity("workspace-edit:sha256:", 'b'),
            1,
            64,
        )
        .unwrap();
        let first = MutationReservationDraft::from_file_write(&request).unwrap();
        let repeated = MutationReservationDraft::from_file_write(&request).unwrap();
        assert_eq!(first, repeated);
        assert_eq!(repeated.retry_disposition(&first), RetryDisposition::Replay);
        assert_eq!(first.request_fingerprint, "a".repeat(64));
        assert_eq!(
            serde_json::from_value::<MutationReservationDraft>(
                serde_json::to_value(&first).unwrap()
            )
            .unwrap(),
            first
        );

        let conflicting_request = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "file-retry-1",
            fingerprint('c'),
            identity("workspace-edit:sha256:", 'd'),
            1,
            64,
        )
        .unwrap();
        let conflicting = MutationReservationDraft::from_file_write(&conflicting_request).unwrap();
        assert_eq!(
            conflicting.retry_disposition(&first),
            RetryDisposition::Conflict
        );

        let unrelated_request = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "file-retry-2",
            fingerprint('c'),
            identity("workspace-edit:sha256:", 'd'),
            1,
            64,
        )
        .unwrap();
        let unrelated = MutationReservationDraft::from_file_write(&unrelated_request).unwrap();
        assert_eq!(
            unrelated.retry_disposition(&first),
            RetryDisposition::Unrelated
        );

        let approval = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "file-retry-1",
            fingerprint('a'),
            Scope::FileChange,
        )
        .unwrap();
        let different_kind = MutationReservationDraft::from_approval(&approval).unwrap();
        assert_eq!(
            different_kind.retry_disposition(&first),
            RetryDisposition::Unrelated
        );
    }

    #[test]
    fn authority_unknown_fields_binding_drift_and_secret_shapes_fail_closed() {
        let request = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "approval-retry-1",
            fingerprint('a'),
            Scope::Permissions,
        )
        .unwrap();
        let draft = MutationReservationDraft::from_approval(&request).unwrap();
        for field in [
            "v20_store_compatible",
            "reservation_persisted",
            "dispatch_authority",
            "mutation_authority",
            "approval_authority",
            "execution_authority",
        ] {
            let mut forged = serde_json::to_value(&draft).unwrap();
            forged[field] = json!(true);
            assert!(serde_json::from_value::<MutationReservationDraft>(forged).is_err());
        }

        let mut unknown = serde_json::to_value(&draft).unwrap();
        unknown["prompt"] = json!("must not enter a reservation");
        assert!(serde_json::from_value::<MutationReservationDraft>(unknown).is_err());

        let mut drifted = serde_json::to_value(&draft).unwrap();
        drifted["session_id"] = json!("api_key=example");
        assert!(serde_json::from_value::<MutationReservationDraft>(drifted).is_err());

        let mut wrong_scope = serde_json::to_value(&draft).unwrap();
        wrong_scope["project_id"] = json!("project-1");
        assert!(serde_json::from_value::<MutationReservationDraft>(wrong_scope).is_err());
    }
}
