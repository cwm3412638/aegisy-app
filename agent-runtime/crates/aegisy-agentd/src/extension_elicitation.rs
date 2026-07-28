//! Metadata-only contract for an extension/MCP elicitation request.
//!
//! This module deliberately records no form, prompt, URL, argument, or user
//! answer content.  It only binds an extension request to a Session and Turn
//! so a future producer can make retries and terminal observations idempotent.
//! The contract is not an approval, permission, execution, or mutation grant.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "extension-elicitation/0.1";
const REQUEST_IDENTITY_PREFIX: &str = "extension-elicitation-request:sha256:";
const OPERATION_IDENTITY_PREFIX: &str = "extension-elicitation-operation:sha256:";
const OBSERVATION_IDENTITY_PREFIX: &str = "extension-elicitation-observation:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtensionElicitationError {
    pub code: &'static str,
    pub message: &'static str,
}

impl ExtensionElicitationError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for ExtensionElicitationError {
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

/// Reject common credential-shaped strings before they can enter an identity
/// or be retained in a serialized metadata record.
fn secret_shaped(value: &str) -> bool {
    if jwt_shaped(value) {
        return true;
    }
    let lowercase = value.to_ascii_lowercase();
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

fn hash_identity(prefix: &str, domain: &[u8], values: &[&str]) -> String {
    let mut digest = Sha256::new();
    digest.update(domain);
    digest.update([0]);
    for value in values {
        digest.update((value.len() as u64).to_be_bytes());
        digest.update(value.as_bytes());
    }
    format!("{prefix}{:x}", digest.finalize())
}

fn request_identity(
    request_id: &str,
    request_fingerprint: &str,
    extension_id: &str,
    kind: ElicitationKind,
) -> Result<String, ExtensionElicitationError> {
    for value in [request_id, extension_id] {
        if !valid_identifier(value) {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-request-invalid",
                "extension elicitation request identifier is invalid",
            ));
        }
    }
    if !valid_sha256_identity(request_fingerprint, "request:sha256:") {
        return Err(ExtensionElicitationError::new(
            "extension-elicitation-fingerprint-invalid",
            "extension elicitation request fingerprint is invalid",
        ));
    }
    Ok(hash_identity(
        REQUEST_IDENTITY_PREFIX,
        b"aegisy-extension-elicitation-request/0.1",
        &[request_id, request_fingerprint, extension_id, kind.as_str()],
    ))
}

fn operation_identity(
    session_id: &str,
    turn_id: &str,
    request_identity: &str,
    idempotency_key: &str,
    request_fingerprint: &str,
) -> Result<String, ExtensionElicitationError> {
    for value in [session_id, turn_id, idempotency_key] {
        if !valid_identifier(value) {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-identity-invalid",
                "extension elicitation operation identity is invalid",
            ));
        }
    }
    if !valid_sha256_identity(request_identity, REQUEST_IDENTITY_PREFIX)
        || !valid_sha256_identity(request_fingerprint, "request:sha256:")
    {
        return Err(ExtensionElicitationError::new(
            "extension-elicitation-binding-invalid",
            "extension elicitation request binding is invalid",
        ));
    }
    Ok(hash_identity(
        OPERATION_IDENTITY_PREFIX,
        b"aegisy-extension-elicitation-operation/0.1",
        &[
            session_id,
            turn_id,
            request_identity,
            idempotency_key,
            request_fingerprint,
        ],
    ))
}

/// The supported MCP elicitation shape is descriptive only.  Neither variant
/// means that Aegisy has accepted or displayed a user decision.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum ElicitationKind {
    Form,
    Url,
}

impl ElicitationKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::Form => "form",
            Self::Url => "url",
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum State {
    /// The exact metadata binding was reserved; no outcome was observed.
    Requested,
    /// A non-decision terminal observation was recorded.
    Resolved,
    /// The request failed without a user answer.
    Failed,
    /// The boundary may have been crossed; only an external reconciler may
    /// determine what happened, and this producer cannot advance the state.
    ReconciliationRequired,
}

impl State {
    fn is_terminal(self) -> bool {
        matches!(self, Self::Resolved | Self::Failed)
    }
}

/// Resolutions intentionally contain no answer, approval, or permission.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Resolution {
    Unresolved,
    Cancelled,
    Expired,
    NotSupported,
    ServerUnavailable,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtensionElicitationRequest {
    pub schema_version: String,
    pub operation_identity: String,
    pub request_identity: String,
    pub session_id: String,
    pub turn_id: String,
    pub request_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub extension_id: String,
    pub kind: ElicitationKind,
    pub user_decision_observed: bool,
    pub permission_authority: bool,
    pub execution_authority: bool,
    pub mutation_authority: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtensionElicitationAcknowledgement {
    pub schema_version: String,
    pub operation_identity: String,
    pub request_identity: String,
    pub session_id: String,
    pub turn_id: String,
    pub request_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub extension_id: String,
    pub kind: ElicitationKind,
    pub revision: u64,
    pub state: State,
    pub resolution: Resolution,
    pub observation_identity: Option<String>,
    pub observed_at_ms: u64,
    pub user_decision_observed: bool,
    pub permission_authority: bool,
    pub execution_authority: bool,
    pub mutation_authority: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RequestWire {
    schema_version: String,
    operation_identity: String,
    request_identity: String,
    session_id: String,
    turn_id: String,
    request_id: String,
    idempotency_key: String,
    request_fingerprint: String,
    extension_id: String,
    kind: ElicitationKind,
    user_decision_observed: bool,
    permission_authority: bool,
    execution_authority: bool,
    mutation_authority: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct AcknowledgementWire {
    schema_version: String,
    operation_identity: String,
    request_identity: String,
    session_id: String,
    turn_id: String,
    request_id: String,
    idempotency_key: String,
    request_fingerprint: String,
    extension_id: String,
    kind: ElicitationKind,
    revision: u64,
    state: State,
    resolution: Resolution,
    observation_identity: Option<String>,
    observed_at_ms: u64,
    user_decision_observed: bool,
    permission_authority: bool,
    execution_authority: bool,
    mutation_authority: bool,
}

impl ExtensionElicitationRequest {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        session_id: impl Into<String>,
        turn_id: impl Into<String>,
        request_id: impl Into<String>,
        idempotency_key: impl Into<String>,
        request_fingerprint: impl Into<String>,
        extension_id: impl Into<String>,
        kind: ElicitationKind,
    ) -> Result<Self, ExtensionElicitationError> {
        let session_id = session_id.into();
        let turn_id = turn_id.into();
        let request_id = request_id.into();
        let idempotency_key = idempotency_key.into();
        let request_fingerprint = request_fingerprint.into();
        let extension_id = extension_id.into();
        let request_identity =
            request_identity(&request_id, &request_fingerprint, &extension_id, kind)?;
        let operation_identity = operation_identity(
            &session_id,
            &turn_id,
            &request_identity,
            &idempotency_key,
            &request_fingerprint,
        )?;
        let request = Self {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity,
            request_identity,
            session_id,
            turn_id,
            request_id,
            idempotency_key,
            request_fingerprint,
            extension_id,
            kind,
            user_decision_observed: false,
            permission_authority: false,
            execution_authority: false,
            mutation_authority: false,
        };
        request.validate()?;
        Ok(request)
    }

    pub fn validate(&self) -> Result<(), ExtensionElicitationError> {
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.operation_identity, OPERATION_IDENTITY_PREFIX)
            || !valid_sha256_identity(&self.request_identity, REQUEST_IDENTITY_PREFIX)
            || self.request_identity
                != request_identity(
                    &self.request_id,
                    &self.request_fingerprint,
                    &self.extension_id,
                    self.kind,
                )?
            || self.operation_identity
                != operation_identity(
                    &self.session_id,
                    &self.turn_id,
                    &self.request_identity,
                    &self.idempotency_key,
                    &self.request_fingerprint,
                )?
        {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-request-invalid",
                "extension elicitation request schema or identity is invalid",
            ));
        }
        for value in [
            &self.session_id,
            &self.turn_id,
            &self.request_id,
            &self.idempotency_key,
            &self.extension_id,
        ] {
            if !valid_identifier(value) {
                return Err(ExtensionElicitationError::new(
                    "extension-elicitation-request-invalid",
                    "extension elicitation request identifier is invalid",
                ));
            }
        }
        if !valid_sha256_identity(&self.request_fingerprint, "request:sha256:") {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-fingerprint-invalid",
                "extension elicitation request fingerprint is invalid",
            ));
        }
        if self.user_decision_observed
            || self.permission_authority
            || self.execution_authority
            || self.mutation_authority
        {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-authority-invalid",
                "extension elicitation metadata cannot grant authority or claim a user decision",
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
    ) -> Result<ExtensionElicitationAcknowledgement, ExtensionElicitationError> {
        self.validate()?;
        let acknowledgement = ExtensionElicitationAcknowledgement {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            request_identity: self.request_identity.clone(),
            session_id: self.session_id.clone(),
            turn_id: self.turn_id.clone(),
            request_id: self.request_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            extension_id: self.extension_id.clone(),
            kind: self.kind,
            revision,
            state,
            resolution,
            observation_identity,
            observed_at_ms,
            user_decision_observed: false,
            permission_authority: false,
            execution_authority: false,
            mutation_authority: false,
        };
        acknowledgement.validate()?;
        Ok(acknowledgement)
    }

    /// Equivalent retries are accepted; a reused key with any changed binding
    /// is a conflict and must not dispatch a second extension request.
    pub fn validate_retry(&self, reserved: &Self) -> Result<(), ExtensionElicitationError> {
        self.validate()?;
        reserved.validate()?;
        if self == reserved {
            Ok(())
        } else {
            Err(ExtensionElicitationError::new(
                "extension-elicitation-idempotency-conflict",
                "extension elicitation idempotency key is already bound to a different request",
            ))
        }
    }
}

impl ExtensionElicitationAcknowledgement {
    pub fn validate(&self) -> Result<(), ExtensionElicitationError> {
        let request = ExtensionElicitationRequest {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            request_identity: self.request_identity.clone(),
            session_id: self.session_id.clone(),
            turn_id: self.turn_id.clone(),
            request_id: self.request_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            extension_id: self.extension_id.clone(),
            kind: self.kind,
            user_decision_observed: self.user_decision_observed,
            permission_authority: self.permission_authority,
            execution_authority: self.execution_authority,
            mutation_authority: self.mutation_authority,
        };
        request.validate()?;
        if self.schema_version != SCHEMA_VERSION
            || self.revision == 0
            || self.revision > MAX_SAFE_JSON_INTEGER
            || self.observed_at_ms == 0
            || self.observed_at_ms > MAX_SAFE_JSON_INTEGER
        {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-ack-bounds-invalid",
                "extension elicitation acknowledgement revision or time is outside its bound",
            ));
        }
        if let Some(identity) = &self.observation_identity {
            if !valid_sha256_identity(identity, OBSERVATION_IDENTITY_PREFIX) {
                return Err(ExtensionElicitationError::new(
                    "extension-elicitation-observation-invalid",
                    "extension elicitation observation identity is invalid",
                ));
            }
        }
        match self.state {
            State::Requested => {
                if self.resolution != Resolution::Unresolved || self.observation_identity.is_some()
                {
                    return Err(ExtensionElicitationError::new(
                        "extension-elicitation-observation-order-invalid",
                        "requested elicitation metadata cannot carry an outcome observation",
                    ));
                }
            }
            State::Resolved => {
                if self.resolution == Resolution::Unresolved || self.observation_identity.is_none()
                {
                    return Err(ExtensionElicitationError::new(
                        "extension-elicitation-resolution-invalid",
                        "resolved elicitation metadata requires a non-decision observation",
                    ));
                }
            }
            State::Failed | State::ReconciliationRequired => {
                if self.resolution != Resolution::Unresolved || self.observation_identity.is_none()
                {
                    return Err(ExtensionElicitationError::new(
                        "extension-elicitation-observation-invalid",
                        "failed or uncertain elicitation metadata requires evidence without a resolution",
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn matches_request(&self, request: &ExtensionElicitationRequest) -> bool {
        self.validate().is_ok()
            && request.validate().is_ok()
            && self.operation_identity == request.operation_identity
            && self.request_identity == request.request_identity
            && self.session_id == request.session_id
            && self.turn_id == request.turn_id
            && self.request_id == request.request_id
            && self.idempotency_key == request.idempotency_key
            && self.request_fingerprint == request.request_fingerprint
            && self.extension_id == request.extension_id
            && self.kind == request.kind
    }

    /// Validates a contiguous, idempotent metadata-only lifecycle transition.
    pub fn can_follow(&self, previous: Option<&Self>) -> Result<(), ExtensionElicitationError> {
        self.validate()?;
        let Some(previous) = previous else {
            return (self.state == State::Requested && self.revision == 1)
                .then_some(())
                .ok_or_else(|| {
                    ExtensionElicitationError::new(
                        "extension-elicitation-state-invalid",
                        "extension elicitation acknowledgement must begin at requested revision one",
                    )
                });
        };
        previous.validate()?;
        if self.operation_identity != previous.operation_identity
            || self.request_identity != previous.request_identity
            || self.session_id != previous.session_id
            || self.turn_id != previous.turn_id
            || self.request_id != previous.request_id
            || self.idempotency_key != previous.idempotency_key
            || self.request_fingerprint != previous.request_fingerprint
            || self.extension_id != previous.extension_id
            || self.kind != previous.kind
        {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-binding-changed",
                "extension elicitation acknowledgement binding changed",
            ));
        }
        if self == previous {
            return Ok(());
        }
        if self.revision == previous.revision {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-idempotency-conflict",
                "extension elicitation acknowledgement revision is already bound to different metadata",
            ));
        }
        if self.revision != previous.revision + 1 {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-revision-invalid",
                "extension elicitation acknowledgement revision is not contiguous",
            ));
        }
        if self.observed_at_ms < previous.observed_at_ms {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-time-invalid",
                "extension elicitation acknowledgement time moved backwards",
            ));
        }
        if previous.state == State::ReconciliationRequired {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-reconciliation-required",
                "uncertain extension elicitation metadata cannot be advanced by this producer",
            ));
        }
        if previous.state.is_terminal() {
            return Err(ExtensionElicitationError::new(
                "extension-elicitation-state-invalid",
                "terminal extension elicitation metadata cannot be advanced",
            ));
        }
        let valid = matches!(
            (previous.state, self.state),
            (State::Requested, State::Resolved)
                | (State::Requested, State::Failed)
                | (State::Requested, State::ReconciliationRequired)
        );
        valid.then_some(()).ok_or_else(|| {
            ExtensionElicitationError::new(
                "extension-elicitation-state-invalid",
                "extension elicitation acknowledgement state moved backwards",
            )
        })
    }
}

impl<'de> Deserialize<'de> for ExtensionElicitationRequest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = RequestWire::deserialize(deserializer)?;
        let request = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            request_identity: wire.request_identity,
            session_id: wire.session_id,
            turn_id: wire.turn_id,
            request_id: wire.request_id,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            extension_id: wire.extension_id,
            kind: wire.kind,
            user_decision_observed: wire.user_decision_observed,
            permission_authority: wire.permission_authority,
            execution_authority: wire.execution_authority,
            mutation_authority: wire.mutation_authority,
        };
        request.validate().map_err(serde::de::Error::custom)?;
        Ok(request)
    }
}

impl Serialize for ExtensionElicitationRequest {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            schema_version: &'a str,
            operation_identity: &'a str,
            request_identity: &'a str,
            session_id: &'a str,
            turn_id: &'a str,
            request_id: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            extension_id: &'a str,
            kind: ElicitationKind,
            user_decision_observed: bool,
            permission_authority: bool,
            execution_authority: bool,
            mutation_authority: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            request_identity: &self.request_identity,
            session_id: &self.session_id,
            turn_id: &self.turn_id,
            request_id: &self.request_id,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            extension_id: &self.extension_id,
            kind: self.kind,
            user_decision_observed: false,
            permission_authority: false,
            execution_authority: false,
            mutation_authority: false,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for ExtensionElicitationAcknowledgement {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = AcknowledgementWire::deserialize(deserializer)?;
        let acknowledgement = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            request_identity: wire.request_identity,
            session_id: wire.session_id,
            turn_id: wire.turn_id,
            request_id: wire.request_id,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            extension_id: wire.extension_id,
            kind: wire.kind,
            revision: wire.revision,
            state: wire.state,
            resolution: wire.resolution,
            observation_identity: wire.observation_identity,
            observed_at_ms: wire.observed_at_ms,
            user_decision_observed: wire.user_decision_observed,
            permission_authority: wire.permission_authority,
            execution_authority: wire.execution_authority,
            mutation_authority: wire.mutation_authority,
        };
        acknowledgement
            .validate()
            .map_err(serde::de::Error::custom)?;
        Ok(acknowledgement)
    }
}

impl Serialize for ExtensionElicitationAcknowledgement {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            schema_version: &'a str,
            operation_identity: &'a str,
            request_identity: &'a str,
            session_id: &'a str,
            turn_id: &'a str,
            request_id: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            extension_id: &'a str,
            kind: ElicitationKind,
            revision: u64,
            state: State,
            resolution: Resolution,
            observation_identity: Option<&'a str>,
            observed_at_ms: u64,
            user_decision_observed: bool,
            permission_authority: bool,
            execution_authority: bool,
            mutation_authority: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            request_identity: &self.request_identity,
            session_id: &self.session_id,
            turn_id: &self.turn_id,
            request_id: &self.request_id,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            extension_id: &self.extension_id,
            kind: self.kind,
            revision: self.revision,
            state: self.state,
            resolution: self.resolution,
            observation_identity: self.observation_identity.as_deref(),
            observed_at_ms: self.observed_at_ms,
            user_decision_observed: false,
            permission_authority: false,
            execution_authority: false,
            mutation_authority: false,
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

    fn observation_identity() -> String {
        format!("{OBSERVATION_IDENTITY_PREFIX}{}", "b".repeat(64))
    }

    fn request() -> ExtensionElicitationRequest {
        ExtensionElicitationRequest::new(
            "session-1",
            "turn-1",
            "mcp-request-1",
            "retry-1",
            fingerprint('1'),
            "mcp:example",
            ElicitationKind::Form,
        )
        .unwrap()
    }

    #[test]
    fn round_trip_is_metadata_only_and_has_no_authority() {
        let request = request();
        let encoded = serde_json::to_value(&request).unwrap();
        assert_eq!(encoded["user_decision_observed"], false);
        assert_eq!(encoded["permission_authority"], false);
        assert_eq!(encoded["execution_authority"], false);
        assert_eq!(encoded["mutation_authority"], false);
        for forbidden in ["prompt", "form", "arguments", "url", "answer"] {
            assert!(encoded.get(forbidden).is_none(), "unexpected {forbidden}");
        }
        let decoded: ExtensionElicitationRequest = serde_json::from_value(encoded).unwrap();
        assert_eq!(decoded, request);
    }

    #[test]
    fn identities_and_equivalent_retries_are_stable() {
        let original = request();
        let equivalent = ExtensionElicitationRequest::new(
            "session-1",
            "turn-1",
            "mcp-request-1",
            "retry-1",
            fingerprint('1'),
            "mcp:example",
            ElicitationKind::Form,
        )
        .unwrap();
        assert_eq!(original, equivalent);
        assert!(original.validate_retry(&equivalent).is_ok());
        let accepted = original
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        let repeated = original
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        assert_eq!(accepted, repeated);
        assert!(accepted.can_follow(None).is_ok());
        assert!(accepted.matches_request(&original));
    }

    #[test]
    fn terminal_observations_are_non_decision_and_contiguous() {
        let request = request();
        let requested = request
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        let resolved = request
            .acknowledgement(
                State::Resolved,
                Resolution::Cancelled,
                2,
                Some(observation_identity()),
                11,
            )
            .unwrap();
        assert!(resolved.can_follow(Some(&requested)).is_ok());
        let changed = request
            .acknowledgement(
                State::Resolved,
                Resolution::Expired,
                2,
                Some(observation_identity()),
                11,
            )
            .unwrap();
        assert!(changed.can_follow(Some(&resolved)).is_err());
        let after_terminal = request
            .acknowledgement(
                State::Failed,
                Resolution::Unresolved,
                3,
                Some(observation_identity()),
                12,
            )
            .unwrap();
        assert!(after_terminal.can_follow(Some(&resolved)).is_err());
    }

    #[test]
    fn binding_drift_uncertainty_and_secrets_fail_closed() {
        let request = request();
        let requested = request
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
            .unwrap();
        let uncertain = request
            .acknowledgement(
                State::ReconciliationRequired,
                Resolution::Unresolved,
                2,
                Some(observation_identity()),
                11,
            )
            .unwrap();
        assert!(uncertain.can_follow(Some(&requested)).is_ok());
        let resolved = request
            .acknowledgement(
                State::Resolved,
                Resolution::Cancelled,
                3,
                Some(observation_identity()),
                12,
            )
            .unwrap();
        assert!(resolved.can_follow(Some(&uncertain)).is_err());

        assert!(ExtensionElicitationRequest::new(
            "session-1",
            "turn-1",
            "authorization-token",
            "retry-1",
            fingerprint('1'),
            "mcp:example",
            ElicitationKind::Form,
        )
        .is_err());
        assert!(ExtensionElicitationRequest::new(
            "session-1",
            "turn-1",
            "mcp-request-1",
            "retry-1",
            fingerprint('1'),
            "mcp:secret-server",
            ElicitationKind::Form,
        )
        .is_err());
    }

    #[test]
    fn forged_authority_or_content_fields_are_rejected() {
        let request = request();
        let mut encoded = serde_json::to_value(&request).unwrap();
        encoded["permission_authority"] = json!(true);
        assert!(serde_json::from_value::<ExtensionElicitationRequest>(encoded).is_err());

        let mut encoded = serde_json::to_value(&request).unwrap();
        encoded["prompt"] = json!("user content");
        assert!(serde_json::from_value::<ExtensionElicitationRequest>(encoded).is_err());

        let mut encoded = serde_json::to_value(
            request
                .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, 10)
                .unwrap(),
        )
        .unwrap();
        encoded["user_decision_observed"] = json!(true);
        assert!(serde_json::from_value::<ExtensionElicitationAcknowledgement>(encoded).is_err());
    }
}
