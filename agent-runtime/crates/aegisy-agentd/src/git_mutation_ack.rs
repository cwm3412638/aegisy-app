//! Metadata-only acknowledgement contract for a reviewed Git mutation.
//!
//! The contract defines retry correlation and an observed lifecycle without
//! executing Git, granting approval, or creating mutation authority. A future
//! Store/AAP producer must persist the reservation before dispatch.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "git-mutation-acknowledgement/0.1";
const OPERATION_IDENTITY_PREFIX: &str = "git-mutation-operation:sha256:";
const OBSERVATION_IDENTITY_PREFIX: &str = "git-mutation-observation:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitMutationAckError {
    pub code: &'static str,
    pub message: &'static str,
}

impl GitMutationAckError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for GitMutationAckError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Kind {
    BranchCreate,
    BranchSwitch,
    BranchRename,
    Stage,
    Commit,
    WorktreeCreate,
    WorktreeRemove,
}

impl Kind {
    fn as_str(self) -> &'static str {
        match self {
            Self::BranchCreate => "branch-create",
            Self::BranchSwitch => "branch-switch",
            Self::BranchRename => "branch-rename",
            Self::Stage => "stage",
            Self::Commit => "commit",
            Self::WorktreeCreate => "worktree-create",
            Self::WorktreeRemove => "worktree-remove",
        }
    }
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RetryDisposition {
    Replay,
    Conflict,
    Unrelated,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitMutationRequest {
    pub schema_version: String,
    pub operation_identity: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub kind: Kind,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub plan_identity: String,
    pub mutation_authority: bool,
    pub approval_authority: bool,
    pub execution_authority: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitMutationAcknowledgement {
    pub schema_version: String,
    pub operation_identity: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub kind: Kind,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub plan_identity: String,
    pub revision: u64,
    pub state: State,
    pub observation_identity: Option<String>,
    pub observed_at_ms: u64,
    pub mutation_authority: bool,
    pub approval_authority: bool,
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
    kind: Kind,
    idempotency_key: String,
    request_fingerprint: String,
    plan_identity: String,
    mutation_authority: bool,
    approval_authority: bool,
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
    kind: Kind,
    idempotency_key: String,
    request_fingerprint: String,
    plan_identity: String,
    revision: u64,
    state: State,
    observation_identity: Option<String>,
    observed_at_ms: u64,
    mutation_authority: bool,
    approval_authority: bool,
    execution_authority: bool,
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_IDENTIFIER_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
        && !secret_shaped(value)
}

fn secret_shaped(value: &str) -> bool {
    let lower = value.to_ascii_lowercase();
    [
        "api_key",
        "apikey",
        "access_token",
        "authorization",
        "bearer",
        "cookie",
        "credential",
        "password",
        "private_key",
        "secret",
    ]
    .iter()
    .any(|marker| lower.contains(marker))
        || value.split('.').count() == 3
            && value.split('.').all(|segment| {
                segment.len() >= 8
                    && segment
                        .bytes()
                        .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
            })
        || value
            .split(|character: char| !character.is_ascii_alphanumeric() && character != '-')
            .any(|token| token.starts_with("sk-") && token.len() >= 20)
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
    kind: Kind,
    idempotency_key: &str,
    request_fingerprint: &str,
    plan_identity: &str,
) -> Result<String, GitMutationAckError> {
    for value in [session_id, project_id, root_id, idempotency_key] {
        if !valid_identifier(value) {
            return Err(GitMutationAckError::new(
                "git-mutation-identity-invalid",
                "Git mutation operation contains an invalid identifier",
            ));
        }
    }
    if !valid_sha256_identity(request_fingerprint, "request:sha256:") {
        return Err(GitMutationAckError::new(
            "git-mutation-fingerprint-invalid",
            "Git mutation request fingerprint is invalid",
        ));
    }
    if !valid_sha256_identity(plan_identity, "git-plan:sha256:") {
        return Err(GitMutationAckError::new(
            "git-mutation-plan-invalid",
            "Git mutation plan identity is invalid",
        ));
    }
    let mut digest = Sha256::new();
    digest.update(b"aegisy-git-mutation-operation/0.1\0");
    for value in [
        session_id,
        project_id,
        root_id,
        kind.as_str(),
        idempotency_key,
        request_fingerprint,
        plan_identity,
    ] {
        digest.update((value.len() as u64).to_be_bytes());
        digest.update(value.as_bytes());
    }
    Ok(format!(
        "{OPERATION_IDENTITY_PREFIX}{:x}",
        digest.finalize()
    ))
}

impl GitMutationRequest {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        session_id: impl Into<String>,
        project_id: impl Into<String>,
        root_id: impl Into<String>,
        kind: Kind,
        idempotency_key: impl Into<String>,
        request_fingerprint: impl Into<String>,
        plan_identity: impl Into<String>,
    ) -> Result<Self, GitMutationAckError> {
        let session_id = session_id.into();
        let project_id = project_id.into();
        let root_id = root_id.into();
        let idempotency_key = idempotency_key.into();
        let request_fingerprint = request_fingerprint.into();
        let plan_identity = plan_identity.into();
        let operation_identity = operation_identity(
            &session_id,
            &project_id,
            &root_id,
            kind,
            &idempotency_key,
            &request_fingerprint,
            &plan_identity,
        )?;
        let request = Self {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity,
            session_id,
            project_id,
            root_id,
            kind,
            idempotency_key,
            request_fingerprint,
            plan_identity,
            mutation_authority: false,
            approval_authority: false,
            execution_authority: false,
        };
        request.validate()?;
        Ok(request)
    }

    pub fn validate(&self) -> Result<(), GitMutationAckError> {
        let expected = operation_identity(
            &self.session_id,
            &self.project_id,
            &self.root_id,
            self.kind,
            &self.idempotency_key,
            &self.request_fingerprint,
            &self.plan_identity,
        )?;
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.operation_identity, OPERATION_IDENTITY_PREFIX)
            || self.operation_identity != expected
        {
            return Err(GitMutationAckError::new(
                "git-mutation-request-invalid",
                "Git mutation request identity or schema is invalid",
            ));
        }
        if self.mutation_authority || self.approval_authority || self.execution_authority {
            return Err(GitMutationAckError::new(
                "git-mutation-authority-invalid",
                "Git mutation acknowledgement cannot grant authority",
            ));
        }
        Ok(())
    }

    pub fn retry_disposition(&self, existing: &Self) -> RetryDisposition {
        if self.session_id != existing.session_id
            || self.project_id != existing.project_id
            || self.root_id != existing.root_id
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

    pub fn acknowledgement(
        &self,
        state: State,
        revision: u64,
        observation_identity: Option<String>,
        observed_at_ms: u64,
    ) -> Result<GitMutationAcknowledgement, GitMutationAckError> {
        self.validate()?;
        let acknowledgement = GitMutationAcknowledgement {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            session_id: self.session_id.clone(),
            project_id: self.project_id.clone(),
            root_id: self.root_id.clone(),
            kind: self.kind,
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            plan_identity: self.plan_identity.clone(),
            revision,
            state,
            observation_identity,
            observed_at_ms,
            mutation_authority: false,
            approval_authority: false,
            execution_authority: false,
        };
        acknowledgement.validate()?;
        Ok(acknowledgement)
    }
}

impl GitMutationAcknowledgement {
    fn request(&self) -> GitMutationRequest {
        GitMutationRequest {
            schema_version: self.schema_version.clone(),
            operation_identity: self.operation_identity.clone(),
            session_id: self.session_id.clone(),
            project_id: self.project_id.clone(),
            root_id: self.root_id.clone(),
            kind: self.kind,
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            plan_identity: self.plan_identity.clone(),
            mutation_authority: self.mutation_authority,
            approval_authority: self.approval_authority,
            execution_authority: self.execution_authority,
        }
    }

    pub fn validate(&self) -> Result<(), GitMutationAckError> {
        self.request().validate()?;
        if self.revision == 0
            || self.revision > MAX_SAFE_JSON_INTEGER
            || self.observed_at_ms == 0
            || self.observed_at_ms > MAX_SAFE_JSON_INTEGER
        {
            return Err(GitMutationAckError::new(
                "git-mutation-ack-bounds-invalid",
                "Git mutation acknowledgement time or revision is outside its bound",
            ));
        }
        if let Some(identity) = &self.observation_identity {
            if !valid_sha256_identity(identity, OBSERVATION_IDENTITY_PREFIX) {
                return Err(GitMutationAckError::new(
                    "git-mutation-observation-invalid",
                    "Git mutation observation identity is invalid",
                ));
            }
        }
        if self.state.is_terminal() != self.observation_identity.is_some() {
            return Err(GitMutationAckError::new(
                "git-mutation-observation-order-invalid",
                "Git mutation terminal evidence does not match its state",
            ));
        }
        Ok(())
    }

    pub fn matches_request(&self, request: &GitMutationRequest) -> bool {
        self.validate().is_ok() && request.validate().is_ok() && self.request() == *request
    }

    pub fn can_follow(&self, previous: Option<&Self>) -> Result<(), GitMutationAckError> {
        self.validate()?;
        let Some(previous) = previous else {
            return (self.state == State::Accepted && self.revision == 1)
                .then_some(())
                .ok_or_else(|| {
                    GitMutationAckError::new(
                        "git-mutation-state-invalid",
                        "Git mutation acknowledgement must begin at accepted revision one",
                    )
                });
        };
        previous.validate()?;
        if self.request() != previous.request() {
            return Err(GitMutationAckError::new(
                "git-mutation-binding-changed",
                "Git mutation acknowledgement binding changed",
            ));
        }
        if self.revision != previous.revision + 1 || self.observed_at_ms < previous.observed_at_ms {
            return Err(GitMutationAckError::new(
                "git-mutation-revision-invalid",
                "Git mutation acknowledgement revision or time is not monotonic",
            ));
        }
        if previous.state == State::ReconciliationRequired {
            return (self.state == State::ReconciliationRequired)
                .then_some(())
                .ok_or_else(|| {
                    GitMutationAckError::new(
                        "git-mutation-reconciliation-required",
                        "Uncertain Git mutation cannot be resolved by this producer",
                    )
                });
        }
        matches!(
            (previous.state, self.state),
            (State::Accepted, State::Accepted)
                | (State::Accepted, State::Committed)
                | (State::Accepted, State::Failed)
                | (State::Accepted, State::ReconciliationRequired)
                | (State::Committed, State::Committed)
                | (State::Failed, State::Failed)
        )
        .then_some(())
        .ok_or_else(|| {
            GitMutationAckError::new(
                "git-mutation-state-invalid",
                "Git mutation acknowledgement state moved backwards",
            )
        })
    }
}

impl<'de> Deserialize<'de> for GitMutationRequest {
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
            kind: wire.kind,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            plan_identity: wire.plan_identity,
            mutation_authority: wire.mutation_authority,
            approval_authority: wire.approval_authority,
            execution_authority: wire.execution_authority,
        };
        request.validate().map_err(serde::de::Error::custom)?;
        Ok(request)
    }
}

impl Serialize for GitMutationRequest {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        RequestWireRef::from(self).serialize(serializer)
    }
}

#[derive(Serialize)]
struct RequestWireRef<'a> {
    schema_version: &'a str,
    operation_identity: &'a str,
    session_id: &'a str,
    project_id: &'a str,
    root_id: &'a str,
    kind: Kind,
    idempotency_key: &'a str,
    request_fingerprint: &'a str,
    plan_identity: &'a str,
    mutation_authority: bool,
    approval_authority: bool,
    execution_authority: bool,
}

impl<'a> From<&'a GitMutationRequest> for RequestWireRef<'a> {
    fn from(request: &'a GitMutationRequest) -> Self {
        Self {
            schema_version: &request.schema_version,
            operation_identity: &request.operation_identity,
            session_id: &request.session_id,
            project_id: &request.project_id,
            root_id: &request.root_id,
            kind: request.kind,
            idempotency_key: &request.idempotency_key,
            request_fingerprint: &request.request_fingerprint,
            plan_identity: &request.plan_identity,
            mutation_authority: false,
            approval_authority: false,
            execution_authority: false,
        }
    }
}

impl<'de> Deserialize<'de> for GitMutationAcknowledgement {
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
            kind: wire.kind,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            plan_identity: wire.plan_identity,
            revision: wire.revision,
            state: wire.state,
            observation_identity: wire.observation_identity,
            observed_at_ms: wire.observed_at_ms,
            mutation_authority: wire.mutation_authority,
            approval_authority: wire.approval_authority,
            execution_authority: wire.execution_authority,
        };
        acknowledgement
            .validate()
            .map_err(serde::de::Error::custom)?;
        Ok(acknowledgement)
    }
}

impl Serialize for GitMutationAcknowledgement {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            #[serde(flatten)]
            request: RequestWireRef<'a>,
            revision: u64,
            state: State,
            observation_identity: Option<&'a str>,
            observed_at_ms: u64,
        }
        Wire {
            request: RequestWireRef::from(&self.request()),
            revision: self.revision,
            state: self.state,
            observation_identity: self.observation_identity.as_deref(),
            observed_at_ms: self.observed_at_ms,
        }
        .serialize(serializer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn request() -> GitMutationRequest {
        GitMutationRequest::new(
            "session-1",
            "project-1",
            "root-1",
            Kind::Commit,
            "retry-1",
            identity("request:sha256:", 'a'),
            identity("git-plan:sha256:", 'b'),
        )
        .unwrap()
    }

    #[test]
    fn equivalent_retries_replay_and_drift_conflicts() {
        let original = request();
        assert_eq!(
            original.retry_disposition(&request()),
            RetryDisposition::Replay
        );
        let drifted = GitMutationRequest::new(
            "session-1",
            "project-1",
            "root-1",
            Kind::Stage,
            "retry-1",
            identity("request:sha256:", 'c'),
            identity("git-plan:sha256:", 'd'),
        )
        .unwrap();
        assert_eq!(
            original.retry_disposition(&drifted),
            RetryDisposition::Conflict
        );
        let unrelated = GitMutationRequest::new(
            "session-1",
            "project-1",
            "root-1",
            Kind::Commit,
            "retry-2",
            identity("request:sha256:", 'a'),
            identity("git-plan:sha256:", 'b'),
        )
        .unwrap();
        assert_eq!(
            original.retry_disposition(&unrelated),
            RetryDisposition::Unrelated
        );
    }

    #[test]
    fn lifecycle_is_contiguous_and_uncertainty_is_sticky() {
        let request = request();
        let accepted = request
            .acknowledgement(State::Accepted, 1, None, 100)
            .unwrap();
        accepted.can_follow(None).unwrap();
        assert!(request
            .acknowledgement(State::Committed, 1, None, 110)
            .is_err());

        let uncertain = request
            .acknowledgement(State::ReconciliationRequired, 2, None, 110)
            .unwrap();
        uncertain.can_follow(Some(&accepted)).unwrap();
        let observed = Some(identity(
            GitMutationAcknowledgement::observation_prefix(),
            'e',
        ));
        let resolved = request
            .acknowledgement(State::Committed, 3, observed, 120)
            .unwrap();
        assert_eq!(
            resolved.can_follow(Some(&uncertain)).unwrap_err().code,
            "git-mutation-reconciliation-required"
        );
    }

    #[test]
    fn wire_rejects_authority_unknown_fields_and_secret_shaped_ids() {
        let request = request();
        let mut value = serde_json::to_value(&request).unwrap();
        value["approval_authority"] = json!(true);
        assert!(serde_json::from_value::<GitMutationRequest>(value).is_err());

        let mut value = serde_json::to_value(&request).unwrap();
        value["extra"] = json!(true);
        assert!(serde_json::from_value::<GitMutationRequest>(value).is_err());

        assert!(GitMutationRequest::new(
            "session-secret-access_token",
            "project-1",
            "root-1",
            Kind::Commit,
            "retry-1",
            identity("request:sha256:", 'a'),
            identity("git-plan:sha256:", 'b'),
        )
        .is_err());
    }

    impl GitMutationAcknowledgement {
        fn observation_prefix() -> &'static str {
            OBSERVATION_IDENTITY_PREFIX
        }
    }
}
