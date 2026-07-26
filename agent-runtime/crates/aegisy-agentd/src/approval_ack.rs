//! Metadata-only acknowledgement contract for an approval-shaped request.
//!
//! This is deliberately not a user Approval producer. It records only a
//! bounded request binding and non-granting observations such as denial or
//! expiry. It is not connected to AAP, the Workbench Store, Qt, Codex, or any
//! permission/execution path.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "approval-acknowledgement/0.1";
const OPERATION_IDENTITY_PREFIX: &str = "approval-operation:sha256:";
const SCOPE_IDENTITY_PREFIX: &str = "approval-scope:sha256:";
const REQUIREMENT_IDENTITY_PREFIX: &str = "approval-requirement:sha256:";
const OBSERVATION_IDENTITY_PREFIX: &str = "approval-observation:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApprovalAckError {
    pub code: &'static str,
    pub message: &'static str,
}

impl ApprovalAckError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for ApprovalAckError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_IDENTIFIER_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
        && !secret_shaped(value)
}

fn secret_shaped(value: &str) -> bool {
    if jwt_shaped(value) {
        return true;
    }
    let lowercase = value.to_ascii_lowercase();
    if [
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
    .any(|marker| lowercase.contains(marker))
    {
        return true;
    }

    value
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

fn valid_sha256_identity(value: &str, prefix: &str) -> bool {
    value.len() == prefix.len() + 64
        && value.starts_with(prefix)
        && value[prefix.len()..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn operation_identity(
    session_id: &str,
    turn_id: &str,
    idempotency_key: &str,
    request_fingerprint: &str,
    scope: Scope,
    scope_identity: &str,
    requirement_identity: &str,
) -> Result<String, ApprovalAckError> {
    for value in [session_id, turn_id, idempotency_key] {
        if !valid_identifier(value) {
            return Err(ApprovalAckError::new(
                "approval-identity-invalid",
                "approval operation identity contains an invalid identifier",
            ));
        }
    }
    for (value, prefix, code, message) in [
        (
            request_fingerprint,
            "request:sha256:",
            "approval-fingerprint-invalid",
            "approval request fingerprint is invalid",
        ),
        (
            scope_identity,
            SCOPE_IDENTITY_PREFIX,
            "approval-scope-invalid",
            "approval scope identity is invalid",
        ),
        (
            requirement_identity,
            REQUIREMENT_IDENTITY_PREFIX,
            "approval-requirement-invalid",
            "approval requirement identity is invalid",
        ),
    ] {
        if !valid_sha256_identity(value, prefix) {
            return Err(ApprovalAckError::new(code, message));
        }
    }
    let mut digest = Sha256::new();
    digest.update(b"aegisy-approval-operation/0.1\0");
    for value in [
        session_id,
        turn_id,
        idempotency_key,
        request_fingerprint,
        scope.as_str(),
        scope_identity,
        requirement_identity,
    ] {
        digest.update((value.len() as u64).to_be_bytes());
        digest.update(value.as_bytes());
    }
    Ok(format!(
        "{OPERATION_IDENTITY_PREFIX}{:x}",
        digest.finalize()
    ))
}

/// Scope is descriptive only. It does not imply that a corresponding action
/// is available or permitted.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Scope {
    CommandExecution,
    FileChange,
    Permissions,
}

impl Scope {
    fn as_str(self) -> &'static str {
        match self {
            Self::CommandExecution => "command-execution",
            Self::FileChange => "file-change",
            Self::Permissions => "permissions",
        }
    }
}

fn scope_identity(
    session_id: &str,
    turn_id: &str,
    scope: Scope,
) -> Result<String, ApprovalAckError> {
    for value in [session_id, turn_id] {
        if !valid_identifier(value) {
            return Err(ApprovalAckError::new(
                "approval-scope-invalid",
                "approval scope contains an invalid identifier",
            ));
        }
    }
    let mut digest = Sha256::new();
    digest.update(b"aegisy-approval-scope/0.1\0");
    for value in [session_id, turn_id, scope.as_str()] {
        digest.update((value.len() as u64).to_be_bytes());
        digest.update(value.as_bytes());
    }
    Ok(format!("{SCOPE_IDENTITY_PREFIX}{:x}", digest.finalize()))
}

fn requirement_identity(
    scope_identity: &str,
    request_fingerprint: &str,
) -> Result<String, ApprovalAckError> {
    if !valid_sha256_identity(scope_identity, SCOPE_IDENTITY_PREFIX)
        || !valid_sha256_identity(request_fingerprint, "request:sha256:")
    {
        return Err(ApprovalAckError::new(
            "approval-requirement-invalid",
            "approval requirement binding is invalid",
        ));
    }
    let mut digest = Sha256::new();
    digest.update(b"aegisy-approval-requirement/0.1\0");
    for value in [scope_identity, request_fingerprint] {
        digest.update((value.len() as u64).to_be_bytes());
        digest.update(value.as_bytes());
    }
    Ok(format!(
        "{REQUIREMENT_IDENTITY_PREFIX}{:x}",
        digest.finalize()
    ))
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum State {
    /// The exact metadata binding was recorded; no outcome was observed.
    Requested,
    /// A non-granting outcome was observed with content-free evidence.
    Resolved,
    /// Observation failed before any outcome could be established.
    Failed,
    /// A boundary may have been crossed, so only an external reconciler may
    /// determine what happened. This producer cannot advance the state.
    ReconciliationRequired,
}

impl State {
    fn is_terminal(self) -> bool {
        matches!(self, Self::Resolved | Self::Failed)
    }
}

/// Resolutions intentionally exclude `allowed`: this contract cannot record
/// or manufacture a user Approval decision.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Resolution {
    Unresolved,
    /// Runtime or policy denied the request. This is not a user decision.
    Denied,
    /// The requirement expired before a user decision was observed.
    Expired,
    /// The requirement was withdrawn or superseded without user approval.
    NotRequired,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApprovalRequest {
    pub schema_version: String,
    pub operation_identity: String,
    pub session_id: String,
    pub turn_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub scope: Scope,
    pub scope_identity: String,
    pub requirement_identity: String,
    pub mutation_authority: bool,
    pub approval_authority: bool,
    pub user_decision_observed: bool,
    pub execution_authority: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApprovalAcknowledgement {
    pub schema_version: String,
    pub operation_identity: String,
    pub session_id: String,
    pub turn_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub scope: Scope,
    pub scope_identity: String,
    pub requirement_identity: String,
    pub revision: u64,
    pub state: State,
    pub resolution: Resolution,
    pub observation_identity: Option<String>,
    pub observed_at_ms: u64,
    pub mutation_authority: bool,
    pub approval_authority: bool,
    pub user_decision_observed: bool,
    pub execution_authority: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RequestWire {
    schema_version: String,
    operation_identity: String,
    session_id: String,
    turn_id: String,
    idempotency_key: String,
    request_fingerprint: String,
    scope: Scope,
    scope_identity: String,
    requirement_identity: String,
    mutation_authority: bool,
    approval_authority: bool,
    user_decision_observed: bool,
    execution_authority: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct AcknowledgementWire {
    schema_version: String,
    operation_identity: String,
    session_id: String,
    turn_id: String,
    idempotency_key: String,
    request_fingerprint: String,
    scope: Scope,
    scope_identity: String,
    requirement_identity: String,
    revision: u64,
    state: State,
    resolution: Resolution,
    observation_identity: Option<String>,
    observed_at_ms: u64,
    mutation_authority: bool,
    approval_authority: bool,
    user_decision_observed: bool,
    execution_authority: bool,
}

impl ApprovalRequest {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        session_id: impl Into<String>,
        turn_id: impl Into<String>,
        idempotency_key: impl Into<String>,
        request_fingerprint: impl Into<String>,
        scope: Scope,
    ) -> Result<Self, ApprovalAckError> {
        let session_id = session_id.into();
        let turn_id = turn_id.into();
        let idempotency_key = idempotency_key.into();
        let request_fingerprint = request_fingerprint.into();
        let scope_identity = scope_identity(&session_id, &turn_id, scope)?;
        let requirement_identity = requirement_identity(&scope_identity, &request_fingerprint)?;
        let operation_identity = operation_identity(
            &session_id,
            &turn_id,
            &idempotency_key,
            &request_fingerprint,
            scope,
            &scope_identity,
            &requirement_identity,
        )?;
        let request = Self {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity,
            session_id,
            turn_id,
            idempotency_key,
            request_fingerprint,
            scope,
            scope_identity,
            requirement_identity,
            mutation_authority: false,
            approval_authority: false,
            user_decision_observed: false,
            execution_authority: false,
        };
        request.validate()?;
        Ok(request)
    }

    pub fn validate(&self) -> Result<(), ApprovalAckError> {
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.operation_identity, OPERATION_IDENTITY_PREFIX)
            || self.operation_identity
                != operation_identity(
                    &self.session_id,
                    &self.turn_id,
                    &self.idempotency_key,
                    &self.request_fingerprint,
                    self.scope,
                    &self.scope_identity,
                    &self.requirement_identity,
                )?
        {
            return Err(ApprovalAckError::new(
                "approval-request-invalid",
                "approval request schema or identity is invalid",
            ));
        }
        if !valid_identifier(&self.session_id)
            || !valid_identifier(&self.turn_id)
            || !valid_identifier(&self.idempotency_key)
        {
            return Err(ApprovalAckError::new(
                "approval-request-invalid",
                "approval request identifier is invalid",
            ));
        }
        let expected_scope_identity = scope_identity(&self.session_id, &self.turn_id, self.scope)?;
        if self.scope_identity != expected_scope_identity {
            return Err(ApprovalAckError::new(
                "approval-scope-invalid",
                "approval scope identity does not match its exact scope binding",
            ));
        }
        let expected_requirement_identity =
            requirement_identity(&self.scope_identity, &self.request_fingerprint)?;
        if self.requirement_identity != expected_requirement_identity {
            return Err(ApprovalAckError::new(
                "approval-requirement-invalid",
                "approval requirement identity does not match its exact request binding",
            ));
        }
        if self.mutation_authority
            || self.approval_authority
            || self.user_decision_observed
            || self.execution_authority
        {
            return Err(ApprovalAckError::new(
                "approval-authority-invalid",
                "approval metadata cannot grant authority or claim a user decision",
            ));
        }
        Ok(())
    }

    pub fn acknowledgement(
        &self,
        state: State,
        resolution: Resolution,
        revision: u64,
        observation_identity: Option<String>,
        observed_at_ms: u64,
    ) -> Result<ApprovalAcknowledgement, ApprovalAckError> {
        self.validate()?;
        let acknowledgement = ApprovalAcknowledgement {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            session_id: self.session_id.clone(),
            turn_id: self.turn_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            scope: self.scope,
            scope_identity: self.scope_identity.clone(),
            requirement_identity: self.requirement_identity.clone(),
            revision,
            state,
            resolution,
            observation_identity,
            observed_at_ms,
            mutation_authority: false,
            approval_authority: false,
            user_decision_observed: false,
            execution_authority: false,
        };
        acknowledgement.validate()?;
        Ok(acknowledgement)
    }

    /// Classifies a request using a previously reserved idempotency key.
    /// Equivalent retries reuse the original operation; any binding drift is a
    /// conflict and cannot dispatch a second approval-shaped operation.
    pub fn validate_retry(&self, reserved: &Self) -> Result<(), ApprovalAckError> {
        self.validate()?;
        reserved.validate()?;
        if self == reserved {
            return Ok(());
        }
        Err(ApprovalAckError::new(
            "approval-idempotency-conflict",
            "approval idempotency key is already bound to a different request",
        ))
    }
}

impl ApprovalAcknowledgement {
    pub fn validate(&self) -> Result<(), ApprovalAckError> {
        let request = ApprovalRequest {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            session_id: self.session_id.clone(),
            turn_id: self.turn_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            scope: self.scope,
            scope_identity: self.scope_identity.clone(),
            requirement_identity: self.requirement_identity.clone(),
            mutation_authority: self.mutation_authority,
            approval_authority: self.approval_authority,
            user_decision_observed: self.user_decision_observed,
            execution_authority: self.execution_authority,
        };
        request.validate()?;
        if self.schema_version != SCHEMA_VERSION
            || self.revision == 0
            || self.revision > MAX_SAFE_JSON_INTEGER
            || self.observed_at_ms == 0
            || self.observed_at_ms > MAX_SAFE_JSON_INTEGER
        {
            return Err(ApprovalAckError::new(
                "approval-ack-bounds-invalid",
                "approval acknowledgement revision or time is outside its bound",
            ));
        }
        if let Some(identity) = &self.observation_identity {
            if !valid_sha256_identity(identity, OBSERVATION_IDENTITY_PREFIX) {
                return Err(ApprovalAckError::new(
                    "approval-observation-invalid",
                    "approval observation identity is invalid",
                ));
            }
        }
        match self.state {
            State::Requested => {
                if self.resolution != Resolution::Unresolved || self.observation_identity.is_some()
                {
                    return Err(ApprovalAckError::new(
                        "approval-observation-order-invalid",
                        "requested approval metadata cannot carry an outcome observation",
                    ));
                }
            }
            State::Resolved => {
                if self.resolution == Resolution::Unresolved || self.observation_identity.is_none()
                {
                    return Err(ApprovalAckError::new(
                        "approval-resolution-invalid",
                        "resolved approval acknowledgement requires a non-granting outcome and observation",
                    ));
                }
            }
            State::Failed => {
                if self.resolution != Resolution::Unresolved || self.observation_identity.is_none()
                {
                    return Err(ApprovalAckError::new(
                        "approval-failure-invalid",
                        "failed approval metadata requires evidence and cannot carry a resolution",
                    ));
                }
            }
            State::ReconciliationRequired => {
                if self.resolution != Resolution::Unresolved || self.observation_identity.is_none()
                {
                    return Err(ApprovalAckError::new(
                        "approval-reconciliation-invalid",
                        "uncertain approval metadata requires evidence and cannot carry a resolution",
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn matches_request(&self, request: &ApprovalRequest) -> bool {
        self.validate().is_ok()
            && request.validate().is_ok()
            && self.operation_identity == request.operation_identity
            && self.session_id == request.session_id
            && self.turn_id == request.turn_id
            && self.idempotency_key == request.idempotency_key
            && self.request_fingerprint == request.request_fingerprint
            && self.scope == request.scope
            && self.scope_identity == request.scope_identity
            && self.requirement_identity == request.requirement_identity
    }

    /// Validates a metadata-only idempotent lifecycle transition.
    pub fn can_follow(&self, previous: Option<&Self>) -> Result<(), ApprovalAckError> {
        self.validate()?;
        let Some(previous) = previous else {
            return (self.state == State::Requested && self.revision == 1)
                .then_some(())
                .ok_or_else(|| {
                    ApprovalAckError::new(
                        "approval-state-invalid",
                        "approval acknowledgement must begin at requested revision one",
                    )
                });
        };
        previous.validate()?;
        if self.operation_identity != previous.operation_identity
            || self.session_id != previous.session_id
            || self.turn_id != previous.turn_id
            || self.idempotency_key != previous.idempotency_key
            || self.request_fingerprint != previous.request_fingerprint
            || self.scope != previous.scope
            || self.scope_identity != previous.scope_identity
            || self.requirement_identity != previous.requirement_identity
        {
            return Err(ApprovalAckError::new(
                "approval-binding-changed",
                "approval acknowledgement binding changed",
            ));
        }
        if self == previous {
            return Ok(());
        }
        if self.revision == previous.revision {
            return Err(ApprovalAckError::new(
                "approval-idempotency-conflict",
                "approval acknowledgement revision is already bound to different metadata",
            ));
        }
        if self.revision != previous.revision + 1 {
            return Err(ApprovalAckError::new(
                "approval-revision-invalid",
                "approval acknowledgement revision is not contiguous",
            ));
        }
        if self.observed_at_ms < previous.observed_at_ms {
            return Err(ApprovalAckError::new(
                "approval-time-invalid",
                "approval acknowledgement time moved backwards",
            ));
        }
        if previous.state == State::ReconciliationRequired {
            return Err(ApprovalAckError::new(
                "approval-reconciliation-required",
                "uncertain approval metadata cannot be advanced by this producer",
            ));
        }
        if previous.state.is_terminal() {
            return Err(ApprovalAckError::new(
                "approval-state-invalid",
                "terminal approval metadata cannot be advanced",
            ));
        }
        let valid = matches!(
            (previous.state, self.state),
            (State::Requested, State::Resolved)
                | (State::Requested, State::Failed)
                | (State::Requested, State::ReconciliationRequired)
        );
        valid.then_some(()).ok_or_else(|| {
            ApprovalAckError::new(
                "approval-state-invalid",
                "approval acknowledgement state moved backwards",
            )
        })
    }
}

impl<'de> Deserialize<'de> for ApprovalRequest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = RequestWire::deserialize(deserializer)?;
        let request = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            session_id: wire.session_id,
            turn_id: wire.turn_id,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            scope: wire.scope,
            scope_identity: wire.scope_identity,
            requirement_identity: wire.requirement_identity,
            mutation_authority: wire.mutation_authority,
            approval_authority: wire.approval_authority,
            user_decision_observed: wire.user_decision_observed,
            execution_authority: wire.execution_authority,
        };
        request.validate().map_err(serde::de::Error::custom)?;
        Ok(request)
    }
}

impl Serialize for ApprovalRequest {
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
            turn_id: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            scope: Scope,
            scope_identity: &'a str,
            requirement_identity: &'a str,
            mutation_authority: bool,
            approval_authority: bool,
            user_decision_observed: bool,
            execution_authority: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            session_id: &self.session_id,
            turn_id: &self.turn_id,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            scope: self.scope,
            scope_identity: &self.scope_identity,
            requirement_identity: &self.requirement_identity,
            mutation_authority: false,
            approval_authority: false,
            user_decision_observed: false,
            execution_authority: false,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for ApprovalAcknowledgement {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = AcknowledgementWire::deserialize(deserializer)?;
        let acknowledgement = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            session_id: wire.session_id,
            turn_id: wire.turn_id,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            scope: wire.scope,
            scope_identity: wire.scope_identity,
            requirement_identity: wire.requirement_identity,
            revision: wire.revision,
            state: wire.state,
            resolution: wire.resolution,
            observation_identity: wire.observation_identity,
            observed_at_ms: wire.observed_at_ms,
            mutation_authority: wire.mutation_authority,
            approval_authority: wire.approval_authority,
            user_decision_observed: wire.user_decision_observed,
            execution_authority: wire.execution_authority,
        };
        acknowledgement
            .validate()
            .map_err(serde::de::Error::custom)?;
        Ok(acknowledgement)
    }
}

impl Serialize for ApprovalAcknowledgement {
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
            turn_id: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            scope: Scope,
            scope_identity: &'a str,
            requirement_identity: &'a str,
            revision: u64,
            state: State,
            resolution: Resolution,
            observation_identity: Option<&'a str>,
            observed_at_ms: u64,
            mutation_authority: bool,
            approval_authority: bool,
            user_decision_observed: bool,
            execution_authority: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            session_id: &self.session_id,
            turn_id: &self.turn_id,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            scope: self.scope,
            scope_identity: &self.scope_identity,
            requirement_identity: &self.requirement_identity,
            revision: self.revision,
            state: self.state,
            resolution: self.resolution,
            observation_identity: self.observation_identity.as_deref(),
            observed_at_ms: self.observed_at_ms,
            mutation_authority: false,
            approval_authority: false,
            user_decision_observed: false,
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

    fn observation_identity(byte: char) -> String {
        format!(
            "approval-observation:sha256:{}",
            byte.to_string().repeat(64)
        )
    }

    fn request() -> ApprovalRequest {
        ApprovalRequest::new(
            "session-1",
            "turn-1",
            "retry-1",
            fingerprint('1'),
            Scope::FileChange,
        )
        .unwrap()
    }

    #[test]
    fn strict_metadata_round_trip_has_no_user_decision_or_authority() {
        let request = request();
        let encoded = serde_json::to_value(&request).unwrap();
        assert_eq!(encoded["mutation_authority"], false);
        assert_eq!(encoded["approval_authority"], false);
        assert_eq!(encoded["user_decision_observed"], false);
        assert_eq!(encoded["execution_authority"], false);
        assert!(encoded.get("prompt").is_none());
        for forbidden in [
            "content",
            "prompt",
            "path",
            "command",
            "provider_body",
            "permission",
            "decision",
        ] {
            assert!(encoded.get(forbidden).is_none());
        }
        let decoded: ApprovalRequest = serde_json::from_value(encoded).unwrap();
        assert_eq!(decoded, request);

        let resolved = request
            .acknowledgement(
                State::Resolved,
                Resolution::Denied,
                2,
                Some(observation_identity('a')),
                11,
            )
            .unwrap();
        let encoded = serde_json::to_value(&resolved).unwrap();
        for forbidden in [
            "content",
            "prompt",
            "path",
            "command",
            "provider_body",
            "permission",
            "decision",
        ] {
            assert!(encoded.get(forbidden).is_none());
        }
        assert_eq!(
            serde_json::from_value::<ApprovalAcknowledgement>(encoded).unwrap(),
            resolved
        );
    }

    #[test]
    fn identities_bind_session_turn_scope_requirement_fingerprint_and_retry() {
        let original = request();
        let same = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "retry-1",
            fingerprint('1'),
            Scope::FileChange,
        )
        .unwrap();
        let different_scope = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "retry-1",
            fingerprint('1'),
            Scope::CommandExecution,
        )
        .unwrap();
        let different_requirement = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "retry-1",
            fingerprint('4'),
            Scope::FileChange,
        )
        .unwrap();
        assert_eq!(original.operation_identity, same.operation_identity);
        assert_eq!(
            original.scope_identity,
            different_requirement.scope_identity
        );
        assert_ne!(
            original.operation_identity,
            different_scope.operation_identity
        );
        assert_ne!(original.scope_identity, different_scope.scope_identity);
        assert_ne!(
            original.requirement_identity,
            different_scope.requirement_identity
        );
        assert_ne!(
            original.requirement_identity,
            different_requirement.requirement_identity
        );

        let mut forged = serde_json::to_value(&original).unwrap();
        forged["scope"] = json!("permissions");
        assert!(serde_json::from_value::<ApprovalRequest>(forged).is_err());
        for field in [
            "operation_identity",
            "scope_identity",
            "requirement_identity",
        ] {
            let mut forged = serde_json::to_value(&original).unwrap();
            let value = forged[field].as_str().unwrap();
            let replacement_suffix = if value.ends_with('0') { '1' } else { '0' };
            let replacement = format!("{}{replacement_suffix}", &value[..value.len() - 1]);
            forged[field] = json!(replacement);
            assert!(serde_json::from_value::<ApprovalRequest>(forged).is_err());
        }
    }

    #[test]
    fn equivalent_retry_reuses_the_operation_and_binding_drift_conflicts() {
        let reserved = request();
        let equivalent: ApprovalRequest =
            serde_json::from_value(serde_json::to_value(&reserved).unwrap()).unwrap();
        assert!(equivalent.validate_retry(&reserved).is_ok());

        let conflicting = ApprovalRequest::new(
            "session-1",
            "turn-1",
            "retry-1",
            fingerprint('9'),
            Scope::FileChange,
        )
        .unwrap();
        let error = conflicting.validate_retry(&reserved).unwrap_err();
        assert_eq!(error.code, "approval-idempotency-conflict");
    }

    #[test]
    fn non_granting_resolutions_have_distinct_strict_semantics() {
        let request = request();
        let requested = request
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        for (index, resolution) in [
            Resolution::Denied,
            Resolution::Expired,
            Resolution::NotRequired,
        ]
        .into_iter()
        .enumerate()
        {
            let resolved = request
                .acknowledgement(
                    State::Resolved,
                    resolution,
                    2,
                    Some(observation_identity(char::from(b'a' + index as u8))),
                    11,
                )
                .unwrap();
            assert!(resolved.can_follow(Some(&requested)).is_ok());
            assert!(!resolved.user_decision_observed);
            assert!(!resolved.approval_authority);
            assert!(!resolved.execution_authority);
            assert!(!resolved.mutation_authority);
        }

        assert!(request
            .acknowledgement(State::Resolved, Resolution::Unresolved, 2, None, 11)
            .is_err());
        assert!(request
            .acknowledgement(
                State::Failed,
                Resolution::Denied,
                2,
                Some(observation_identity('d')),
                11,
            )
            .is_err());
        assert!(request
            .acknowledgement(
                State::ReconciliationRequired,
                Resolution::Denied,
                2,
                Some(observation_identity('e')),
                11,
            )
            .is_err());
    }

    #[test]
    fn revisions_are_contiguous_and_exact_replays_do_not_advance_state() {
        let request = request();
        let requested = request
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        assert!(requested.can_follow(None).is_ok());
        assert!(requested.can_follow(Some(&requested)).is_ok());

        let denied = request
            .acknowledgement(
                State::Resolved,
                Resolution::Denied,
                2,
                Some(observation_identity('b')),
                11,
            )
            .unwrap();
        assert!(denied.can_follow(Some(&requested)).is_ok());
        assert!(denied.can_follow(Some(&denied)).is_ok());

        let same_revision_conflict = request
            .acknowledgement(
                State::Resolved,
                Resolution::Expired,
                2,
                Some(observation_identity('c')),
                11,
            )
            .unwrap();
        assert_eq!(
            same_revision_conflict
                .can_follow(Some(&denied))
                .unwrap_err()
                .code,
            "approval-idempotency-conflict"
        );

        let advanced_terminal = request
            .acknowledgement(
                State::Resolved,
                Resolution::Denied,
                3,
                Some(observation_identity('b')),
                12,
            )
            .unwrap();
        assert!(advanced_terminal.can_follow(Some(&denied)).is_err());

        let skipped = request
            .acknowledgement(
                State::Failed,
                Resolution::Unresolved,
                3,
                Some(observation_identity('d')),
                11,
            )
            .unwrap();
        assert!(skipped.can_follow(Some(&requested)).is_err());

        let backwards = request
            .acknowledgement(
                State::Failed,
                Resolution::Unresolved,
                2,
                Some(observation_identity('d')),
                9,
            )
            .unwrap();
        assert!(backwards.can_follow(Some(&requested)).is_err());
    }

    #[test]
    fn reconciliation_required_is_evidenced_and_producer_terminal() {
        let request = request();
        let requested = request
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        assert!(request
            .acknowledgement(
                State::ReconciliationRequired,
                Resolution::Unresolved,
                2,
                None,
                11,
            )
            .is_err());
        let uncertain = request
            .acknowledgement(
                State::ReconciliationRequired,
                Resolution::Unresolved,
                2,
                Some(observation_identity('e')),
                11,
            )
            .unwrap();
        assert!(uncertain.can_follow(Some(&requested)).is_ok());
        assert!(uncertain.can_follow(Some(&uncertain)).is_ok());

        for state in [
            State::Resolved,
            State::Failed,
            State::ReconciliationRequired,
        ] {
            let resolution = if state == State::Resolved {
                Resolution::Denied
            } else {
                Resolution::Unresolved
            };
            let next = request
                .acknowledgement(state, resolution, 3, Some(observation_identity('f')), 12)
                .unwrap();
            assert_eq!(
                next.can_follow(Some(&uncertain)).unwrap_err().code,
                "approval-reconciliation-required"
            );
        }
    }

    #[test]
    fn unknown_fields_authority_and_grant_shaped_values_fail_closed() {
        let request = request();
        let requested = request
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        let mut forged = serde_json::to_value(requested).unwrap();
        forged["user_decision_observed"] = json!(true);
        assert!(serde_json::from_value::<ApprovalAcknowledgement>(forged).is_err());

        let mut unknown_ack = serde_json::to_value(
            request
                .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
                .unwrap(),
        )
        .unwrap();
        unknown_ack["provider_body"] = json!("do not persist");
        assert!(serde_json::from_value::<ApprovalAcknowledgement>(unknown_ack).is_err());

        let mut unknown = serde_json::to_value(request).unwrap();
        unknown["prompt"] = json!("do not persist");
        assert!(serde_json::from_value::<ApprovalRequest>(unknown).is_err());
        assert!(serde_json::from_value::<Resolution>(json!("allowed")).is_err());
        assert!(serde_json::from_value::<Resolution>(json!("approved")).is_err());
        assert!(serde_json::from_value::<Scope>(json!("network-access")).is_err());
    }

    #[test]
    fn bounds_and_secret_shaped_identifiers_fail_closed() {
        for idempotency_key in [
            "api_key=example",
            "sk-123456789012345678901234",
            "ghp_123456789012345678901234",
            "abcdefgh.ijklmnop.qrstuvwx",
            "retry=abcdefgh.ijklmnop.qrstuvwx",
            "prefix:abcdefgh.ijklmnop.qrstuvwx:suffix",
        ] {
            assert!(ApprovalRequest::new(
                "session-1",
                "turn-1",
                idempotency_key,
                fingerprint('1'),
                Scope::Permissions,
            )
            .is_err());
        }
        for idempotency_key in ["retry.v1", "com.example.project", "session.alpha.build-1"] {
            assert!(ApprovalRequest::new(
                "session-1",
                "turn-1",
                idempotency_key,
                fingerprint('1'),
                Scope::Permissions,
            )
            .is_ok());
        }
        assert!(ApprovalRequest::new(
            "",
            "turn-1",
            "retry-1",
            fingerprint('1'),
            Scope::Permissions,
        )
        .is_err());
        assert!(ApprovalRequest::new(
            "s".repeat(MAX_IDENTIFIER_BYTES + 1),
            "turn-1",
            "retry-1",
            fingerprint('1'),
            Scope::Permissions,
        )
        .is_err());

        let request = request();
        for (revision, observed_at_ms) in [
            (0, 10),
            (MAX_SAFE_JSON_INTEGER + 1, 10),
            (1, 0),
            (1, MAX_SAFE_JSON_INTEGER + 1),
        ] {
            assert!(request
                .acknowledgement(
                    State::Requested,
                    Resolution::Unresolved,
                    revision,
                    None,
                    observed_at_ms,
                )
                .is_err());
        }
    }
}
