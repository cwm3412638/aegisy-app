//! Metadata-only acknowledgement contract for a user-reviewed file write.
//!
//! This module is intentionally not connected to the filesystem, Workbench
//! Store, AAP, or Qt. It records bounded identities and an observed lifecycle
//! so a future producer can correlate retries without turning an observation
//! into permission, approval, or execution authority.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "file-write-acknowledgement/0.1";
const OPERATION_IDENTITY_PREFIX: &str = "file-write-operation:sha256:";
const OBSERVATION_IDENTITY_PREFIX: &str = "file-write-observation:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_IDENTITY_BYTES: usize = 128;
const MAX_CHANGED_FILES: u64 = 256;
const MAX_BYTES: u64 = 4 * 1024 * 1024;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileWriteAckError {
    pub code: &'static str,
    pub message: &'static str,
}

impl FileWriteAckError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for FileWriteAckError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_IDENTIFIER_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
}

fn valid_sha256_identity(value: &str, prefix: &str) -> bool {
    value.len() == prefix.len() + 64
        && value.starts_with(prefix)
        && value[prefix.len()..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn operation_identity(
    session_id: &str,
    project_id: &str,
    root_id: &str,
    idempotency_key: &str,
    request_fingerprint: &str,
    edit_identity: &str,
) -> Result<String, FileWriteAckError> {
    for value in [session_id, project_id, root_id, idempotency_key] {
        if !valid_identifier(value) {
            return Err(FileWriteAckError::new(
                "file-write-identity-invalid",
                "file-write operation identity contains an invalid identifier",
            ));
        }
    }
    if !valid_sha256_identity(request_fingerprint, "request:sha256:") {
        return Err(FileWriteAckError::new(
            "file-write-fingerprint-invalid",
            "file-write request fingerprint is invalid",
        ));
    }
    if !valid_sha256_identity(edit_identity, "workspace-edit:sha256:") {
        return Err(FileWriteAckError::new(
            "file-write-edit-identity-invalid",
            "file-write edit identity is invalid",
        ));
    }
    let mut digest = Sha256::new();
    digest.update(b"aegisy-file-write-operation/0.1\0");
    for value in [
        session_id,
        project_id,
        root_id,
        idempotency_key,
        request_fingerprint,
        edit_identity,
    ] {
        digest.update((value.len() as u64).to_be_bytes());
        digest.update(value.as_bytes());
    }
    Ok(format!(
        "{OPERATION_IDENTITY_PREFIX}{:x}",
        digest.finalize()
    ))
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum State {
    Accepted,
    Committed,
    Failed,
    ReconciliationRequired,
}

impl State {
    fn is_terminal(self) -> bool {
        matches!(self, Self::Committed | Self::Failed)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileWriteRequest {
    pub schema_version: String,
    pub operation_identity: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub edit_identity: String,
    pub changed_files: u64,
    pub requested_bytes: u64,
    pub mutation_authority: bool,
    pub execution_authority: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileWriteAcknowledgement {
    pub schema_version: String,
    pub operation_identity: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub edit_identity: String,
    pub changed_files: u64,
    pub requested_bytes: u64,
    pub revision: u64,
    pub state: State,
    pub observation_identity: Option<String>,
    pub observed_at_ms: u64,
    pub mutation_authority: bool,
    pub execution_authority: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RequestWire {
    schema_version: String,
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
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct AcknowledgementWire {
    schema_version: String,
    operation_identity: String,
    session_id: String,
    project_id: String,
    root_id: String,
    idempotency_key: String,
    request_fingerprint: String,
    edit_identity: String,
    changed_files: u64,
    requested_bytes: u64,
    revision: u64,
    state: State,
    observation_identity: Option<String>,
    observed_at_ms: u64,
    mutation_authority: bool,
    execution_authority: bool,
}

impl FileWriteRequest {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        session_id: impl Into<String>,
        project_id: impl Into<String>,
        root_id: impl Into<String>,
        idempotency_key: impl Into<String>,
        request_fingerprint: impl Into<String>,
        edit_identity: impl Into<String>,
        changed_files: u64,
        requested_bytes: u64,
    ) -> Result<Self, FileWriteAckError> {
        let session_id = session_id.into();
        let project_id = project_id.into();
        let root_id = root_id.into();
        let idempotency_key = idempotency_key.into();
        let request_fingerprint = request_fingerprint.into();
        let edit_identity = edit_identity.into();
        let operation_identity = operation_identity(
            &session_id,
            &project_id,
            &root_id,
            &idempotency_key,
            &request_fingerprint,
            &edit_identity,
        )?;
        let request = Self {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity,
            session_id,
            project_id,
            root_id,
            idempotency_key,
            request_fingerprint,
            edit_identity,
            changed_files,
            requested_bytes,
            mutation_authority: false,
            execution_authority: false,
        };
        request.validate()?;
        Ok(request)
    }

    pub fn validate(&self) -> Result<(), FileWriteAckError> {
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.operation_identity, OPERATION_IDENTITY_PREFIX)
            || self.operation_identity
                != operation_identity(
                    &self.session_id,
                    &self.project_id,
                    &self.root_id,
                    &self.idempotency_key,
                    &self.request_fingerprint,
                    &self.edit_identity,
                )?
        {
            return Err(FileWriteAckError::new(
                "file-write-request-invalid",
                "file-write request identity or schema is invalid",
            ));
        }
        for value in [
            &self.session_id,
            &self.project_id,
            &self.root_id,
            &self.idempotency_key,
            &self.edit_identity,
        ] {
            if !valid_identifier(value) {
                return Err(FileWriteAckError::new(
                    "file-write-request-invalid",
                    "file-write request identifier is invalid",
                ));
            }
        }
        if self.edit_identity.len() > MAX_IDENTITY_BYTES
            || !valid_sha256_identity(&self.edit_identity, "workspace-edit:sha256:")
        {
            return Err(FileWriteAckError::new(
                "file-write-edit-identity-invalid",
                "file-write edit identity is invalid",
            ));
        }
        if self.changed_files == 0 || self.changed_files > MAX_CHANGED_FILES {
            return Err(FileWriteAckError::new(
                "file-write-bounds-invalid",
                "file-write changed-file count is outside its bound",
            ));
        }
        if self.requested_bytes == 0 || self.requested_bytes > MAX_BYTES {
            return Err(FileWriteAckError::new(
                "file-write-bounds-invalid",
                "file-write byte count is outside its bound",
            ));
        }
        if self.mutation_authority || self.execution_authority {
            return Err(FileWriteAckError::new(
                "file-write-authority-invalid",
                "file-write acknowledgement cannot grant authority",
            ));
        }
        Ok(())
    }

    pub fn acknowledgement(
        &self,
        state: State,
        revision: u64,
        observation_identity: Option<String>,
        observed_at_ms: u64,
    ) -> Result<FileWriteAcknowledgement, FileWriteAckError> {
        self.validate()?;
        let acknowledgement = FileWriteAcknowledgement {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            session_id: self.session_id.clone(),
            project_id: self.project_id.clone(),
            root_id: self.root_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            edit_identity: self.edit_identity.clone(),
            changed_files: self.changed_files,
            requested_bytes: self.requested_bytes,
            revision,
            state,
            observation_identity,
            observed_at_ms,
            mutation_authority: false,
            execution_authority: false,
        };
        acknowledgement.validate()?;
        Ok(acknowledgement)
    }
}

impl FileWriteAcknowledgement {
    pub fn validate(&self) -> Result<(), FileWriteAckError> {
        let request = FileWriteRequest {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            session_id: self.session_id.clone(),
            project_id: self.project_id.clone(),
            root_id: self.root_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            edit_identity: self.edit_identity.clone(),
            changed_files: self.changed_files,
            requested_bytes: self.requested_bytes,
            mutation_authority: self.mutation_authority,
            execution_authority: self.execution_authority,
        };
        request.validate()?;
        if self.schema_version != SCHEMA_VERSION || self.revision == 0 {
            return Err(FileWriteAckError::new(
                "file-write-ack-invalid",
                "file-write acknowledgement schema or revision is invalid",
            ));
        }
        if self.revision > MAX_SAFE_JSON_INTEGER
            || self.observed_at_ms == 0
            || self.observed_at_ms > MAX_SAFE_JSON_INTEGER
        {
            return Err(FileWriteAckError::new(
                "file-write-ack-bounds-invalid",
                "file-write acknowledgement time or revision is outside its bound",
            ));
        }
        if let Some(identity) = &self.observation_identity {
            if !valid_sha256_identity(identity, OBSERVATION_IDENTITY_PREFIX) {
                return Err(FileWriteAckError::new(
                    "file-write-observation-invalid",
                    "file-write observation identity is invalid",
                ));
            }
        }
        if self.state.is_terminal() && self.observation_identity.is_none() {
            return Err(FileWriteAckError::new(
                "file-write-observation-missing",
                "terminal file-write acknowledgement requires an observation identity",
            ));
        }
        if matches!(self.state, State::Accepted | State::ReconciliationRequired)
            && self.observation_identity.is_some()
        {
            return Err(FileWriteAckError::new(
                "file-write-observation-order-invalid",
                "non-terminal file-write acknowledgement cannot carry terminal evidence",
            ));
        }
        Ok(())
    }

    pub fn matches_request(&self, request: &FileWriteRequest) -> bool {
        self.validate().is_ok()
            && request.validate().is_ok()
            && self.operation_identity == request.operation_identity
            && self.session_id == request.session_id
            && self.project_id == request.project_id
            && self.root_id == request.root_id
            && self.idempotency_key == request.idempotency_key
            && self.request_fingerprint == request.request_fingerprint
            && self.edit_identity == request.edit_identity
    }

    /// Validates an idempotent lifecycle transition without dispatching work.
    pub fn can_follow(&self, previous: Option<&Self>) -> Result<(), FileWriteAckError> {
        self.validate()?;
        let Some(previous) = previous else {
            return (self.state == State::Accepted && self.revision == 1)
                .then_some(())
                .ok_or_else(|| {
                    FileWriteAckError::new(
                        "file-write-state-invalid",
                        "file-write acknowledgement must begin at accepted revision one",
                    )
                });
        };
        previous.validate()?;
        if self.operation_identity != previous.operation_identity
            || self.session_id != previous.session_id
            || self.project_id != previous.project_id
            || self.root_id != previous.root_id
            || self.idempotency_key != previous.idempotency_key
            || self.request_fingerprint != previous.request_fingerprint
            || self.edit_identity != previous.edit_identity
        {
            return Err(FileWriteAckError::new(
                "file-write-binding-changed",
                "file-write acknowledgement binding changed",
            ));
        }
        if self.revision != previous.revision + 1 {
            return Err(FileWriteAckError::new(
                "file-write-revision-invalid",
                "file-write acknowledgement revision is not contiguous",
            ));
        }
        if self.observed_at_ms < previous.observed_at_ms {
            return Err(FileWriteAckError::new(
                "file-write-time-invalid",
                "file-write acknowledgement time moved backwards",
            ));
        }
        if previous.state == State::ReconciliationRequired {
            return (self.state == State::ReconciliationRequired)
                .then_some(())
                .ok_or_else(|| {
                    FileWriteAckError::new(
                        "file-write-reconciliation-required",
                        "uncertain file-write acknowledgement cannot be resolved by this producer",
                    )
                });
        }
        let valid = matches!(
            (previous.state, self.state),
            (State::Accepted, State::Accepted)
                | (State::Accepted, State::Committed)
                | (State::Accepted, State::Failed)
                | (State::Accepted, State::ReconciliationRequired)
                | (State::Committed, State::Committed)
                | (State::Failed, State::Failed)
        );
        valid.then_some(()).ok_or_else(|| {
            FileWriteAckError::new(
                "file-write-state-invalid",
                "file-write acknowledgement state moved backwards",
            )
        })
    }
}

impl<'de> Deserialize<'de> for FileWriteRequest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = RequestWire::deserialize(deserializer)?;
        let request = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            session_id: wire.session_id,
            project_id: wire.project_id,
            root_id: wire.root_id,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            edit_identity: wire.edit_identity,
            changed_files: wire.changed_files,
            requested_bytes: wire.requested_bytes,
            mutation_authority: wire.mutation_authority,
            execution_authority: wire.execution_authority,
        };
        request.validate().map_err(serde::de::Error::custom)?;
        Ok(request)
    }
}

impl Serialize for FileWriteRequest {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            schema_version: &'a str,
            operation_identity: &'a str,
            session_id: &'a str,
            project_id: &'a str,
            root_id: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            edit_identity: &'a str,
            changed_files: u64,
            requested_bytes: u64,
            mutation_authority: bool,
            execution_authority: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            session_id: &self.session_id,
            project_id: &self.project_id,
            root_id: &self.root_id,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            edit_identity: &self.edit_identity,
            changed_files: self.changed_files,
            requested_bytes: self.requested_bytes,
            mutation_authority: false,
            execution_authority: false,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for FileWriteAcknowledgement {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = AcknowledgementWire::deserialize(deserializer)?;
        let acknowledgement = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            session_id: wire.session_id,
            project_id: wire.project_id,
            root_id: wire.root_id,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            edit_identity: wire.edit_identity,
            changed_files: wire.changed_files,
            requested_bytes: wire.requested_bytes,
            revision: wire.revision,
            state: wire.state,
            observation_identity: wire.observation_identity,
            observed_at_ms: wire.observed_at_ms,
            mutation_authority: wire.mutation_authority,
            execution_authority: wire.execution_authority,
        };
        acknowledgement
            .validate()
            .map_err(serde::de::Error::custom)?;
        Ok(acknowledgement)
    }
}

impl Serialize for FileWriteAcknowledgement {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            schema_version: &'a str,
            operation_identity: &'a str,
            session_id: &'a str,
            project_id: &'a str,
            root_id: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            edit_identity: &'a str,
            changed_files: u64,
            requested_bytes: u64,
            revision: u64,
            state: State,
            observation_identity: Option<&'a str>,
            observed_at_ms: u64,
            mutation_authority: bool,
            execution_authority: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            session_id: &self.session_id,
            project_id: &self.project_id,
            root_id: &self.root_id,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            edit_identity: &self.edit_identity,
            changed_files: self.changed_files,
            requested_bytes: self.requested_bytes,
            revision: self.revision,
            state: self.state,
            observation_identity: self.observation_identity.as_deref(),
            observed_at_ms: self.observed_at_ms,
            mutation_authority: false,
            execution_authority: false,
        }
        .serialize(serializer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn fingerprint(byte: char) -> String {
        format!("request:sha256:{}", byte.to_string().repeat(64))
    }

    fn edit_identity() -> String {
        format!("workspace-edit:sha256:{}", "a".repeat(64))
    }

    fn observation_identity() -> String {
        format!("file-write-observation:sha256:{}", "b".repeat(64))
    }

    fn request() -> FileWriteRequest {
        FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "retry-1",
            fingerprint('1'),
            edit_identity(),
            2,
            128,
        )
        .unwrap()
    }

    #[test]
    fn strict_metadata_round_trip_has_no_content_or_authority() {
        let request = request();
        let encoded = serde_json::to_value(&request).unwrap();
        assert_eq!(encoded["mutation_authority"], false);
        assert_eq!(encoded["execution_authority"], false);
        assert!(encoded.get("path").is_none());
        assert!(encoded.get("content").is_none());
        let decoded: FileWriteRequest = serde_json::from_value(encoded).unwrap();
        assert_eq!(decoded, request);
    }

    #[test]
    fn operation_identity_and_idempotent_retries_are_stable() {
        let request = request();
        let accepted = request
            .acknowledgement(State::Accepted, 1, None, 10)
            .unwrap();
        let repeated = request
            .acknowledgement(State::Accepted, 2, None, 10)
            .unwrap();
        let committed = request
            .acknowledgement(State::Committed, 2, Some(observation_identity()), 11)
            .unwrap();
        assert_eq!(accepted.operation_identity, repeated.operation_identity);
        assert!(accepted.can_follow(None).is_ok());
        assert!(repeated.can_follow(Some(&accepted)).is_ok());
        assert!(committed.can_follow(Some(&accepted)).is_ok());
        assert!(committed.matches_request(&request));
    }

    #[test]
    fn same_key_and_fingerprint_with_different_edit_identity_is_not_idempotent() {
        let original = request();
        let different_edit = FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "retry-1",
            fingerprint('1'),
            format!("workspace-edit:sha256:{}", "c".repeat(64)),
            2,
            128,
        )
        .unwrap();
        assert_ne!(
            original.operation_identity,
            different_edit.operation_identity
        );
        let accepted = original
            .acknowledgement(State::Accepted, 1, None, 10)
            .unwrap();
        let conflicting = different_edit
            .acknowledgement(State::Accepted, 2, None, 11)
            .unwrap();
        assert!(conflicting.can_follow(Some(&accepted)).is_err());
    }

    #[test]
    fn binding_state_and_revision_drift_fail_closed() {
        let request = request();
        let accepted = request
            .acknowledgement(State::Accepted, 1, None, 10)
            .unwrap();
        let mut wrong_binding = request
            .acknowledgement(State::Accepted, 2, None, 11)
            .unwrap();
        wrong_binding.root_id = "root-2".into();
        assert!(wrong_binding.can_follow(Some(&accepted)).is_err());
        let skipped = request
            .acknowledgement(State::Committed, 3, Some(observation_identity()), 12)
            .unwrap();
        assert!(skipped.can_follow(Some(&accepted)).is_err());
        let uncertain = request
            .acknowledgement(State::ReconciliationRequired, 2, None, 11)
            .unwrap();
        let resolved = request
            .acknowledgement(State::Committed, 3, Some(observation_identity()), 12)
            .unwrap();
        assert!(uncertain.can_follow(Some(&accepted)).is_ok());
        assert!(resolved.can_follow(Some(&uncertain)).is_err());
    }

    #[test]
    fn bounds_authority_and_unknown_fields_fail_closed() {
        assert!(FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "retry-1",
            fingerprint('1'),
            edit_identity(),
            0,
            1,
        )
        .is_err());
        assert!(FileWriteRequest::new(
            "session-1",
            "project-1",
            "root-1",
            "retry-1",
            fingerprint('1'),
            edit_identity(),
            1,
            MAX_BYTES + 1,
        )
        .is_err());
        let request = request();
        let accepted = request
            .acknowledgement(State::Accepted, 1, None, 10)
            .unwrap();
        let mut encoded = serde_json::to_value(accepted).unwrap();
        encoded["execution_authority"] = json!(true);
        assert!(serde_json::from_value::<FileWriteAcknowledgement>(encoded).is_err());
        let unknown = json!({
            "schema_version": SCHEMA_VERSION,
            "operation_identity": "file-write-operation:sha256:".to_owned() + &"a".repeat(64),
            "session_id": "session-1",
            "project_id": "project-1",
            "root_id": "root-1",
            "idempotency_key": "retry-1",
            "request_fingerprint": fingerprint('1'),
            "edit_identity": edit_identity(),
            "changed_files": 1,
            "requested_bytes": 1,
            "mutation_authority": false,
            "execution_authority": false,
            "path": "secret.txt"
        });
        assert!(serde_json::from_value::<FileWriteRequest>(unknown).is_err());
    }
}
