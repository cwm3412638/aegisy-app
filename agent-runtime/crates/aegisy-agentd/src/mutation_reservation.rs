//! Content-free sources and reservation drafts for non-Turn mutation-shaped requests.
//!
//! Schema-v20's durable mutation ledger is intentionally Turn-specific: its
//! only kind is `turn-start`, and every observed transition binds Turn Timeline
//! anchors. Schema v22 accepts one complete validated approval, file-write, Git,
//! or background-job source and derives the existing lossy draft internally.
//! The Store persists both as non-authorizing reservation evidence; it never
//! reconstructs a source from a draft or accepts an independently supplied pair.
//! Neither representation advertises AAP, dispatches work, or grants authority.

use crate::approval_ack::{ApprovalRequest, Scope as ApprovalScope};
use crate::background_job::{BackgroundJobRequest, JobRetryPolicy, JobSchedule};
use crate::file_write_ack::FileWriteRequest;
use crate::git_mutation_ack::{GitMutationRequest, Kind as GitMutationKind};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "mutation-reservation-draft/0.1";
const RESERVATION_IDENTITY_PREFIX: &str = "mutation-reservation-draft:sha256:";
const SOURCE_IDENTITY_PREFIX: &str = "mutation-reservation-source:sha256:";
const MAX_SOURCE_JSON_BYTES: usize = 16 * 1024;
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

/// The complete validated request that produced a reservation draft.
///
/// Serialization is deliberately transparent: the resulting JSON is the exact
/// canonical JSON of the wrapped request, without an additional enum envelope.
/// The request schema version is the variant discriminator on decode.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MutationReservationSource {
    Approval(ApprovalRequest),
    FileWrite(FileWriteRequest),
    GitMutation(GitMutationRequest),
    JobSubmission(BackgroundJobRequest),
}

#[derive(Debug, Deserialize)]
#[serde(tag = "schema_version", deny_unknown_fields)]
enum MutationReservationSourceWire {
    #[serde(rename = "approval-acknowledgement/0.1")]
    Approval {
        operation_identity: String,
        session_id: String,
        turn_id: String,
        idempotency_key: String,
        request_fingerprint: String,
        scope: ApprovalScope,
        scope_identity: String,
        requirement_identity: String,
        mutation_authority: bool,
        approval_authority: bool,
        user_decision_observed: bool,
        execution_authority: bool,
    },
    #[serde(rename = "file-write-acknowledgement/0.1")]
    FileWrite {
        operation_identity: String,
        session_id: String,
        project_id: String,
        root_id: String,
        idempotency_key: String,
        request_fingerprint: String,
        edit_identity: String,
        changed_files: u64,
        requested_bytes: u64,
        mutation_authority: bool,
        execution_authority: bool,
    },
    #[serde(rename = "git-mutation-acknowledgement/0.1")]
    GitMutation {
        operation_identity: String,
        session_id: String,
        project_id: String,
        root_id: String,
        kind: GitMutationKind,
        idempotency_key: String,
        request_fingerprint: String,
        plan_identity: String,
        mutation_authority: bool,
        approval_authority: bool,
        execution_authority: bool,
    },
    #[serde(rename = "background-job-request/0.1")]
    JobSubmission {
        job_id: String,
        session_id: String,
        project_id: String,
        root_id: String,
        execution_plan_identity: String,
        idempotency_identity: String,
        child_task_identity: Option<String>,
        schedule: JobSchedule,
        retry: JobRetryPolicy,
        created_at_ms: u64,
    },
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

impl MutationReservationSource {
    pub fn from_approval(request: ApprovalRequest) -> Result<Self, MutationReservationError> {
        let source = Self::Approval(request);
        source.validate()?;
        Ok(source)
    }

    pub fn from_file_write(request: FileWriteRequest) -> Result<Self, MutationReservationError> {
        let source = Self::FileWrite(request);
        source.validate()?;
        Ok(source)
    }

    pub fn from_git_mutation(
        request: GitMutationRequest,
    ) -> Result<Self, MutationReservationError> {
        let source = Self::GitMutation(request);
        source.validate()?;
        Ok(source)
    }

    pub fn from_job_submission(
        request: BackgroundJobRequest,
    ) -> Result<Self, MutationReservationError> {
        let source = Self::JobSubmission(request);
        source.validate()?;
        Ok(source)
    }

    pub fn kind(&self) -> MutationReservationKind {
        match self {
            Self::Approval(_) => MutationReservationKind::Approval,
            Self::FileWrite(_) => MutationReservationKind::FileWrite,
            Self::GitMutation(_) => MutationReservationKind::GitMutation,
            Self::JobSubmission(_) => MutationReservationKind::JobSubmission,
        }
    }

    fn derive_draft(&self) -> Result<MutationReservationDraft, MutationReservationError> {
        if let Self::JobSubmission(request) = self {
            // Job identifiers are otherwise checked only for their alphabet. The
            // complete source retains job_id even though the lossy draft does not.
            if secret_shaped(&request.job_id) {
                return Err(MutationReservationError::new(
                    "mutation-reservation-source-secret",
                    "mutation reservation source contains secret-shaped metadata",
                ));
            }
        }
        match self {
            Self::Approval(request) => MutationReservationDraft::from_approval(request),
            Self::FileWrite(request) => MutationReservationDraft::from_file_write(request),
            Self::GitMutation(request) => MutationReservationDraft::from_git_mutation(request),
            Self::JobSubmission(request) => MutationReservationDraft::from_job_submission(request),
        }
    }

    pub fn validate(&self) -> Result<(), MutationReservationError> {
        self.derive_draft().map(|_| ())
    }

    /// Recreates the existing lossy draft without changing its schema or identity.
    pub fn to_draft(&self) -> Result<MutationReservationDraft, MutationReservationError> {
        self.derive_draft()
    }

    /// Returns the bounded compact JSON bytes of the complete wrapped request.
    pub fn canonical_bytes(&self) -> Result<Vec<u8>, MutationReservationError> {
        self.validate()?;
        let bytes = serde_json::to_vec(self).map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-serialize-failed",
                "mutation reservation source could not be serialized",
            )
        })?;
        if bytes.len() > MAX_SOURCE_JSON_BYTES {
            return Err(MutationReservationError::new(
                "mutation-reservation-source-size-exceeded",
                "mutation reservation source exceeds its JSON byte bound",
            ));
        }
        let decoded: Self = serde_json::from_slice(&bytes).map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-round-trip-invalid",
                "mutation reservation source could not be decoded after serialization",
            )
        })?;
        let reencoded = serde_json::to_vec(&decoded).map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-round-trip-invalid",
                "mutation reservation source could not be re-encoded",
            )
        })?;
        if decoded != *self || reencoded != bytes {
            return Err(MutationReservationError::new(
                "mutation-reservation-source-round-trip-invalid",
                "mutation reservation source changed during canonical serialization",
            ));
        }
        Ok(bytes)
    }

    /// Decodes only exact canonical request JSON, rejecting whitespace, field
    /// order, or representation drift as well as over-limit and invalid input.
    pub fn from_canonical_bytes(bytes: &[u8]) -> Result<Self, MutationReservationError> {
        if bytes.len() > MAX_SOURCE_JSON_BYTES {
            return Err(MutationReservationError::new(
                "mutation-reservation-source-size-exceeded",
                "mutation reservation source exceeds its JSON byte bound",
            ));
        }
        let source: Self = serde_json::from_slice(bytes).map_err(|_| {
            MutationReservationError::new(
                "mutation-reservation-source-invalid",
                "mutation reservation source JSON is invalid",
            )
        })?;
        let canonical = source.canonical_bytes()?;
        if canonical != bytes {
            return Err(MutationReservationError::new(
                "mutation-reservation-source-canonical-invalid",
                "mutation reservation source JSON is not canonical",
            ));
        }
        Ok(source)
    }

    pub fn canonical_sha256(&self) -> Result<String, MutationReservationError> {
        Ok(format!("{:x}", Sha256::digest(self.canonical_bytes()?)))
    }

    pub fn source_identity(&self) -> Result<String, MutationReservationError> {
        let canonical = self.canonical_bytes()?;
        let kind = self.kind().as_str().as_bytes();
        let mut digest = Sha256::new();
        digest.update(b"aegisy-mutation-reservation-source/0.1\0");
        for value in [kind, canonical.as_slice()] {
            digest.update((value.len() as u64).to_be_bytes());
            digest.update(value);
        }
        Ok(format!("{SOURCE_IDENTITY_PREFIX}{:x}", digest.finalize()))
    }
}

impl Serialize for MutationReservationSource {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        match self {
            Self::Approval(request) => request.serialize(serializer),
            Self::FileWrite(request) => request.serialize(serializer),
            Self::GitMutation(request) => request.serialize(serializer),
            Self::JobSubmission(request) => request.serialize(serializer),
        }
    }
}

impl<'de> Deserialize<'de> for MutationReservationSource {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let source = match MutationReservationSourceWire::deserialize(deserializer)? {
            MutationReservationSourceWire::Approval {
                operation_identity,
                session_id,
                turn_id,
                idempotency_key,
                request_fingerprint,
                scope,
                scope_identity,
                requirement_identity,
                mutation_authority,
                approval_authority,
                user_decision_observed,
                execution_authority,
            } => Self::Approval(ApprovalRequest {
                schema_version: crate::approval_ack::SCHEMA_VERSION.into(),
                operation_identity,
                session_id,
                turn_id,
                idempotency_key,
                request_fingerprint,
                scope,
                scope_identity,
                requirement_identity,
                mutation_authority,
                approval_authority,
                user_decision_observed,
                execution_authority,
            }),
            MutationReservationSourceWire::FileWrite {
                operation_identity,
                session_id,
                project_id,
                root_id,
                idempotency_key,
                request_fingerprint,
                edit_identity,
                changed_files,
                requested_bytes,
                mutation_authority,
                execution_authority,
            } => Self::FileWrite(FileWriteRequest {
                schema_version: crate::file_write_ack::SCHEMA_VERSION.into(),
                operation_identity,
                session_id,
                project_id,
                root_id,
                idempotency_key,
                request_fingerprint,
                edit_identity,
                changed_files,
                requested_bytes,
                mutation_authority,
                execution_authority,
            }),
            MutationReservationSourceWire::GitMutation {
                operation_identity,
                session_id,
                project_id,
                root_id,
                kind,
                idempotency_key,
                request_fingerprint,
                plan_identity,
                mutation_authority,
                approval_authority,
                execution_authority,
            } => Self::GitMutation(GitMutationRequest {
                schema_version: crate::git_mutation_ack::SCHEMA_VERSION.into(),
                operation_identity,
                session_id,
                project_id,
                root_id,
                kind,
                idempotency_key,
                request_fingerprint,
                plan_identity,
                mutation_authority,
                approval_authority,
                execution_authority,
            }),
            MutationReservationSourceWire::JobSubmission {
                job_id,
                session_id,
                project_id,
                root_id,
                execution_plan_identity,
                idempotency_identity,
                child_task_identity,
                schedule,
                retry,
                created_at_ms,
            } => Self::JobSubmission(BackgroundJobRequest {
                schema_version: crate::background_job::REQUEST_SCHEMA_VERSION.into(),
                job_id,
                session_id,
                project_id,
                root_id,
                execution_plan_identity,
                idempotency_identity,
                child_task_identity,
                schedule,
                retry,
                created_at_ms,
            }),
        };
        source.validate().map_err(serde::de::Error::custom)?;
        Ok(source)
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

    fn job_request() -> BackgroundJobRequest {
        BackgroundJobRequest {
            schema_version: crate::background_job::REQUEST_SCHEMA_VERSION.into(),
            job_id: "job-1".into(),
            session_id: "session-1".into(),
            project_id: "project-1".into(),
            root_id: "root-1".into(),
            execution_plan_identity: identity("unified-execution-plan:sha256:", 'f'),
            idempotency_identity: identity("idempotency:sha256:", '1'),
            child_task_identity: Some(identity("child-task:sha256:", '2')),
            schedule: JobSchedule {
                kind: JobScheduleKind::At,
                scheduled_for_ms: Some(20),
            },
            retry: JobRetryPolicy {
                max_attempts: 2,
                backoff_ms: 50,
                safe_retry_boundary_identity: Some(identity("retry-boundary:sha256:", '3')),
            },
            created_at_ms: 10,
        }
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

    #[test]
    fn complete_sources_preserve_request_bytes_and_existing_drafts() {
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
        let job = job_request();

        let cases = [
            (
                MutationReservationSource::from_approval(approval.clone()).unwrap(),
                serde_json::to_vec(&approval).unwrap(),
                MutationReservationDraft::from_approval(&approval).unwrap(),
            ),
            (
                MutationReservationSource::from_file_write(file_write.clone()).unwrap(),
                serde_json::to_vec(&file_write).unwrap(),
                MutationReservationDraft::from_file_write(&file_write).unwrap(),
            ),
            (
                MutationReservationSource::from_git_mutation(git.clone()).unwrap(),
                serde_json::to_vec(&git).unwrap(),
                MutationReservationDraft::from_git_mutation(&git).unwrap(),
            ),
            (
                MutationReservationSource::from_job_submission(job.clone()).unwrap(),
                serde_json::to_vec(&job).unwrap(),
                MutationReservationDraft::from_job_submission(&job).unwrap(),
            ),
        ];

        assert_eq!(
            cases
                .iter()
                .map(|(source, _, _)| source.kind())
                .collect::<Vec<_>>(),
            vec![
                MutationReservationKind::Approval,
                MutationReservationKind::FileWrite,
                MutationReservationKind::GitMutation,
                MutationReservationKind::JobSubmission,
            ]
        );
        for (source, request_bytes, existing_draft) in cases {
            source.validate().unwrap();
            assert_eq!(serde_json::to_vec(&source).unwrap(), request_bytes);
            assert_eq!(source.canonical_bytes().unwrap(), request_bytes);
            assert_eq!(
                MutationReservationSource::from_canonical_bytes(&request_bytes).unwrap(),
                source
            );
            assert_eq!(source.to_draft().unwrap(), existing_draft);
            assert_eq!(
                serde_json::to_vec(&source.to_draft().unwrap()).unwrap(),
                serde_json::to_vec(&existing_draft).unwrap()
            );
            assert_eq!(
                source.canonical_sha256().unwrap(),
                format!("{:x}", Sha256::digest(&request_bytes))
            );
            let source_identity = source.source_identity().unwrap();
            assert!(valid_sha256_identity(
                &source_identity,
                SOURCE_IDENTITY_PREFIX
            ));
            assert_eq!(source.source_identity().unwrap(), source_identity);
        }
    }

    #[test]
    fn complete_file_source_detects_drift_hidden_by_the_existing_draft() {
        let first_request = FileWriteRequest::new(
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
        let drifted_request = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "file-retry-1",
            fingerprint('a'),
            identity("workspace-edit:sha256:", 'b'),
            2,
            128,
        )
        .unwrap();
        let first = MutationReservationSource::from_file_write(first_request).unwrap();
        let drifted = MutationReservationSource::from_file_write(drifted_request).unwrap();

        assert_eq!(first.to_draft().unwrap(), drifted.to_draft().unwrap());
        assert_ne!(first, drifted);
        assert_ne!(
            first.canonical_bytes().unwrap(),
            drifted.canonical_bytes().unwrap()
        );
        assert_ne!(
            first.canonical_sha256().unwrap(),
            drifted.canonical_sha256().unwrap()
        );
        assert_ne!(
            first.source_identity().unwrap(),
            drifted.source_identity().unwrap()
        );
    }

    #[test]
    fn source_decode_rejects_unknown_authority_secret_and_nested_job_fields() {
        let approval = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "approval-retry-1",
            fingerprint('a'),
            Scope::Permissions,
        )
        .unwrap();
        let approval = MutationReservationSource::from_approval(approval).unwrap();
        for field in [
            "dispatch_authority",
            "mutation_authority",
            "approval_authority",
            "user_decision_observed",
            "execution_authority",
        ] {
            let mut forged = serde_json::to_value(&approval).unwrap();
            forged[field] = json!(true);
            assert!(serde_json::from_value::<MutationReservationSource>(forged).is_err());
        }

        let job = MutationReservationSource::from_job_submission(job_request()).unwrap();
        for field in [
            "prompt",
            "dispatch_authority",
            "mutation_authority",
            "approval_authority",
            "execution_authority",
        ] {
            let mut unknown = serde_json::to_value(&job).unwrap();
            unknown[field] = json!(true);
            assert!(serde_json::from_value::<MutationReservationSource>(unknown).is_err());
        }
        let mut schedule_unknown = serde_json::to_value(&job).unwrap();
        schedule_unknown["schedule"]["command"] = json!("must-not-run");
        assert!(serde_json::from_value::<MutationReservationSource>(schedule_unknown).is_err());
        let mut retry_unknown = serde_json::to_value(&job).unwrap();
        retry_unknown["retry"]["token"] = json!("must-not-persist");
        assert!(serde_json::from_value::<MutationReservationSource>(retry_unknown).is_err());

        let mut secret_job = job_request();
        secret_job.job_id = "api_key-secret-job".into();
        secret_job.validate().unwrap();
        let secret_source = MutationReservationSource::JobSubmission(secret_job);
        let error = secret_source.validate().unwrap_err();
        assert_eq!(error.code, "mutation-reservation-source-secret");
        assert!(serde_json::to_vec(&secret_source).is_err());

        let mut unsupported_schema = serde_json::to_value(&job).unwrap();
        unsupported_schema["schema_version"] = json!("background-job-request/9.9");
        assert!(serde_json::from_value::<MutationReservationSource>(unsupported_schema).is_err());

        let mut wrong_schema = serde_json::to_value(&approval).unwrap();
        wrong_schema["schema_version"] = json!(crate::background_job::REQUEST_SCHEMA_VERSION);
        assert!(serde_json::from_value::<MutationReservationSource>(wrong_schema).is_err());

        for field in ["job_id", "session_id", "project_id", "root_id"] {
            let mut request = job_request();
            match field {
                "job_id" => request.job_id = "api_key_secret".into(),
                "session_id" => request.session_id = "api_key_secret".into(),
                "project_id" => request.project_id = "api_key_secret".into(),
                "root_id" => request.root_id = "api_key_secret".into(),
                _ => unreachable!(),
            }
            request.validate().unwrap();
            assert!(MutationReservationSource::JobSubmission(request)
                .validate()
                .is_err());
        }
    }

    #[test]
    fn canonical_source_input_enforces_utf8_byte_bound_and_exact_encoding() {
        let source = MutationReservationSource::from_job_submission(job_request()).unwrap();
        let canonical = source.canonical_bytes().unwrap();

        let mut padded = vec![b' '];
        padded.extend_from_slice(&canonical);
        let error = MutationReservationSource::from_canonical_bytes(&padded).unwrap_err();
        assert_eq!(error.code, "mutation-reservation-source-canonical-invalid");

        let mut exact_limit = canonical.clone();
        exact_limit.resize(MAX_SOURCE_JSON_BYTES, b' ');
        let error = MutationReservationSource::from_canonical_bytes(&exact_limit).unwrap_err();
        assert_eq!(error.code, "mutation-reservation-source-canonical-invalid");
        exact_limit.push(b' ');
        let error = MutationReservationSource::from_canonical_bytes(&exact_limit).unwrap_err();
        assert_eq!(error.code, "mutation-reservation-source-size-exceeded");

        let oversized_utf8 = "界".repeat(MAX_SOURCE_JSON_BYTES / 3 + 1);
        assert!(oversized_utf8.chars().count() < oversized_utf8.len());
        let error =
            MutationReservationSource::from_canonical_bytes(oversized_utf8.as_bytes()).unwrap_err();
        assert_eq!(error.code, "mutation-reservation-source-size-exceeded");

        let error = MutationReservationSource::from_canonical_bytes(&[0xff]).unwrap_err();
        assert_eq!(error.code, "mutation-reservation-source-invalid");
    }
}
