//! Metadata-only contract for an observed credential-refresh request.
//!
//! This module deliberately does not refresh credentials. It carries only
//! bounded, content-free identities and an observation lifecycle that a
//! future host-owned producer could reconcile. It is not connected to AAP,
//! Qt, the Workbench Store, secure storage, a network client, or an authority
//! issuer. Credential values, tokens, provider responses, and refresh/network
//! permissions are absent from this contract.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "credential-refresh-request/0.1";
const OPERATION_IDENTITY_PREFIX: &str = "credential-refresh-operation:sha256:";
const CREDENTIAL_IDENTITY_PREFIX: &str = "credential:sha256:";
const OBSERVATION_IDENTITY_PREFIX: &str = "credential-refresh-observation:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CredentialRefreshError {
    pub code: &'static str,
    pub message: &'static str,
}

impl CredentialRefreshError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for CredentialRefreshError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
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

/// Conservative rejection for values that look like a secret, even when a
/// caller attempts to place them in a descriptive identity field.
fn secret_shaped(value: &str) -> bool {
    if jwt_shaped(value) {
        return true;
    }
    let lowercase = value.to_ascii_lowercase();
    if [
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
        "token",
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
                || (token.starts_with("xoxb-") && token.len() >= 20)
                || jwt_shaped(token)
        })
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_IDENTIFIER_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
        && !secret_shaped(value)
}

fn valid_sha256_identity(value: &str, prefix: &str) -> bool {
    value.len() == prefix.len() + 64
        && value.starts_with(prefix)
        && value[prefix.len()..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn operation_identity(
    provider_id: &str,
    profile_id: &str,
    credential_identity: &str,
    idempotency_key: &str,
    request_fingerprint: &str,
) -> Result<String, CredentialRefreshError> {
    for value in [provider_id, profile_id, idempotency_key] {
        if !valid_identifier(value) {
            return Err(CredentialRefreshError::new(
                "credential-refresh-identity-invalid",
                "credential refresh operation identity contains an invalid identifier",
            ));
        }
    }
    if !valid_sha256_identity(credential_identity, CREDENTIAL_IDENTITY_PREFIX) {
        return Err(CredentialRefreshError::new(
            "credential-refresh-credential-identity-invalid",
            "credential refresh credential identity is invalid",
        ));
    }
    if !valid_sha256_identity(request_fingerprint, "request:sha256:") {
        return Err(CredentialRefreshError::new(
            "credential-refresh-fingerprint-invalid",
            "credential refresh request fingerprint is invalid",
        ));
    }

    let mut digest = Sha256::new();
    digest.update(b"aegisy-credential-refresh-operation/0.1\0");
    for value in [
        provider_id,
        profile_id,
        credential_identity,
        idempotency_key,
        request_fingerprint,
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
    /// The metadata binding was accepted; no refresh observation exists.
    Requested,
    /// A non-authorizing host observation was recorded.
    Observed,
    /// The observation failed before a refresh outcome could be established.
    Failed,
    /// A boundary may have been crossed; only an external reconciler may
    /// determine what happened. This producer cannot advance this state.
    ReconciliationRequired,
}

impl State {
    fn is_terminal(self) -> bool {
        matches!(self, Self::Observed | Self::Failed)
    }
}

/// Outcomes are deliberately limited to content-free observations. There is
/// no `refreshed`, `credential`, or `token` outcome because this contract has
/// no authority to obtain or report a credential value.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Observation {
    Unresolved,
    NotConfigured,
    Unavailable,
    Denied,
    Expired,
    Unchanged,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CredentialRefreshRequest {
    pub schema_version: String,
    pub operation_identity: String,
    pub provider_id: String,
    pub profile_id: String,
    /// A one-way identity for the secure-storage entry, never its value.
    pub credential_identity: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub refresh_authority: bool,
    pub network_authority: bool,
    pub credential_value_present: bool,
    pub credential_value_retained: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CredentialRefreshObservation {
    pub schema_version: String,
    pub operation_identity: String,
    pub provider_id: String,
    pub profile_id: String,
    pub credential_identity: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub revision: u64,
    pub state: State,
    pub observation: Observation,
    pub observation_identity: Option<String>,
    pub observed_at_ms: u64,
    pub refresh_authority: bool,
    pub network_authority: bool,
    pub credential_value_present: bool,
    pub credential_value_retained: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RequestWire {
    schema_version: String,
    operation_identity: String,
    provider_id: String,
    profile_id: String,
    credential_identity: String,
    idempotency_key: String,
    request_fingerprint: String,
    refresh_authority: bool,
    network_authority: bool,
    credential_value_present: bool,
    credential_value_retained: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ObservationWire {
    schema_version: String,
    operation_identity: String,
    provider_id: String,
    profile_id: String,
    credential_identity: String,
    idempotency_key: String,
    request_fingerprint: String,
    revision: u64,
    state: State,
    observation: Observation,
    observation_identity: Option<String>,
    observed_at_ms: u64,
    refresh_authority: bool,
    network_authority: bool,
    credential_value_present: bool,
    credential_value_retained: bool,
}

impl CredentialRefreshRequest {
    pub fn new(
        provider_id: impl Into<String>,
        profile_id: impl Into<String>,
        credential_identity: impl Into<String>,
        idempotency_key: impl Into<String>,
        request_fingerprint: impl Into<String>,
    ) -> Result<Self, CredentialRefreshError> {
        let provider_id = provider_id.into();
        let profile_id = profile_id.into();
        let credential_identity = credential_identity.into();
        let idempotency_key = idempotency_key.into();
        let request_fingerprint = request_fingerprint.into();
        let operation_identity = operation_identity(
            &provider_id,
            &profile_id,
            &credential_identity,
            &idempotency_key,
            &request_fingerprint,
        )?;
        let request = Self {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity,
            provider_id,
            profile_id,
            credential_identity,
            idempotency_key,
            request_fingerprint,
            refresh_authority: false,
            network_authority: false,
            credential_value_present: false,
            credential_value_retained: false,
        };
        request.validate()?;
        Ok(request)
    }

    pub fn validate(&self) -> Result<(), CredentialRefreshError> {
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.operation_identity, OPERATION_IDENTITY_PREFIX)
            || self.operation_identity
                != operation_identity(
                    &self.provider_id,
                    &self.profile_id,
                    &self.credential_identity,
                    &self.idempotency_key,
                    &self.request_fingerprint,
                )?
        {
            return Err(CredentialRefreshError::new(
                "credential-refresh-request-invalid",
                "credential refresh request schema or identity is invalid",
            ));
        }
        for value in [&self.provider_id, &self.profile_id, &self.idempotency_key] {
            if !valid_identifier(value) {
                return Err(CredentialRefreshError::new(
                    "credential-refresh-request-invalid",
                    "credential refresh request identifier is invalid",
                ));
            }
        }
        if !valid_sha256_identity(&self.credential_identity, CREDENTIAL_IDENTITY_PREFIX) {
            return Err(CredentialRefreshError::new(
                "credential-refresh-credential-identity-invalid",
                "credential refresh credential identity is invalid",
            ));
        }
        if !valid_sha256_identity(&self.request_fingerprint, "request:sha256:") {
            return Err(CredentialRefreshError::new(
                "credential-refresh-fingerprint-invalid",
                "credential refresh request fingerprint is invalid",
            ));
        }
        if self.refresh_authority
            || self.network_authority
            || self.credential_value_present
            || self.credential_value_retained
        {
            return Err(CredentialRefreshError::new(
                "credential-refresh-authority-invalid",
                "credential refresh metadata cannot grant authority or carry a credential value",
            ));
        }
        Ok(())
    }

    pub fn observation(
        &self,
        state: State,
        observation: Observation,
        revision: u64,
        observation_identity: Option<String>,
        observed_at_ms: u64,
    ) -> Result<CredentialRefreshObservation, CredentialRefreshError> {
        self.validate()?;
        let result = CredentialRefreshObservation {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            provider_id: self.provider_id.clone(),
            profile_id: self.profile_id.clone(),
            credential_identity: self.credential_identity.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            revision,
            state,
            observation,
            observation_identity,
            observed_at_ms,
            refresh_authority: false,
            network_authority: false,
            credential_value_present: false,
            credential_value_retained: false,
        };
        result.validate()?;
        Ok(result)
    }

    /// Equivalent retries are accepted only when every request binding is
    /// identical; a reused key with different metadata cannot dispatch.
    pub fn validate_retry(&self, reserved: &Self) -> Result<(), CredentialRefreshError> {
        self.validate()?;
        reserved.validate()?;
        if self == reserved {
            return Ok(());
        }
        Err(CredentialRefreshError::new(
            "credential-refresh-idempotency-conflict",
            "credential refresh idempotency key is already bound to different metadata",
        ))
    }
}

impl CredentialRefreshObservation {
    pub fn validate(&self) -> Result<(), CredentialRefreshError> {
        let request = CredentialRefreshRequest {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            provider_id: self.provider_id.clone(),
            profile_id: self.profile_id.clone(),
            credential_identity: self.credential_identity.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            refresh_authority: self.refresh_authority,
            network_authority: self.network_authority,
            credential_value_present: self.credential_value_present,
            credential_value_retained: self.credential_value_retained,
        };
        request.validate()?;
        if self.schema_version != SCHEMA_VERSION
            || self.revision == 0
            || self.revision > MAX_SAFE_JSON_INTEGER
            || self.observed_at_ms == 0
            || self.observed_at_ms > MAX_SAFE_JSON_INTEGER
        {
            return Err(CredentialRefreshError::new(
                "credential-refresh-observation-bounds-invalid",
                "credential refresh observation revision or time is outside its bound",
            ));
        }
        if let Some(identity) = &self.observation_identity {
            if !valid_sha256_identity(identity, OBSERVATION_IDENTITY_PREFIX) {
                return Err(CredentialRefreshError::new(
                    "credential-refresh-observation-identity-invalid",
                    "credential refresh observation identity is invalid",
                ));
            }
        }
        match self.state {
            State::Requested => {
                if self.observation != Observation::Unresolved
                    || self.observation_identity.is_some()
                {
                    return Err(CredentialRefreshError::new(
                        "credential-refresh-observation-order-invalid",
                        "requested credential refresh metadata cannot carry an observation",
                    ));
                }
            }
            State::Observed => {
                if self.observation == Observation::Unresolved
                    || self.observation_identity.is_none()
                {
                    return Err(CredentialRefreshError::new(
                        "credential-refresh-observation-invalid",
                        "observed credential refresh metadata requires bounded evidence",
                    ));
                }
            }
            State::Failed | State::ReconciliationRequired => {
                if self.observation != Observation::Unresolved
                    || self.observation_identity.is_none()
                {
                    return Err(CredentialRefreshError::new(
                        "credential-refresh-observation-invalid",
                        "failed or uncertain credential refresh metadata requires evidence and no outcome",
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn matches_request(&self, request: &CredentialRefreshRequest) -> bool {
        self.validate().is_ok()
            && request.validate().is_ok()
            && self.operation_identity == request.operation_identity
            && self.provider_id == request.provider_id
            && self.profile_id == request.profile_id
            && self.credential_identity == request.credential_identity
            && self.idempotency_key == request.idempotency_key
            && self.request_fingerprint == request.request_fingerprint
    }

    /// Validates one metadata-only lifecycle transition. Exact replays are
    /// idempotent; uncertain or terminal observations cannot be advanced by
    /// this producer.
    pub fn can_follow(&self, previous: Option<&Self>) -> Result<(), CredentialRefreshError> {
        self.validate()?;
        let Some(previous) = previous else {
            return (self.state == State::Requested && self.revision == 1)
                .then_some(())
                .ok_or_else(|| {
                    CredentialRefreshError::new(
                        "credential-refresh-state-invalid",
                        "credential refresh observation must begin at requested revision one",
                    )
                });
        };
        previous.validate()?;
        if self.operation_identity != previous.operation_identity
            || self.provider_id != previous.provider_id
            || self.profile_id != previous.profile_id
            || self.credential_identity != previous.credential_identity
            || self.idempotency_key != previous.idempotency_key
            || self.request_fingerprint != previous.request_fingerprint
        {
            return Err(CredentialRefreshError::new(
                "credential-refresh-binding-changed",
                "credential refresh observation binding changed",
            ));
        }
        if self == previous {
            return Ok(());
        }
        if self.revision == previous.revision {
            return Err(CredentialRefreshError::new(
                "credential-refresh-idempotency-conflict",
                "credential refresh observation revision is already bound to different metadata",
            ));
        }
        if self.revision != previous.revision + 1 {
            return Err(CredentialRefreshError::new(
                "credential-refresh-revision-invalid",
                "credential refresh observation revision is not contiguous",
            ));
        }
        if self.observed_at_ms < previous.observed_at_ms {
            return Err(CredentialRefreshError::new(
                "credential-refresh-time-invalid",
                "credential refresh observation time moved backwards",
            ));
        }
        if previous.state == State::ReconciliationRequired {
            return Err(CredentialRefreshError::new(
                "credential-refresh-reconciliation-required",
                "uncertain credential refresh metadata cannot be advanced by this producer",
            ));
        }
        if previous.state.is_terminal() {
            return Err(CredentialRefreshError::new(
                "credential-refresh-state-invalid",
                "terminal credential refresh metadata cannot be advanced",
            ));
        }
        let valid = matches!(
            (previous.state, self.state),
            (State::Requested, State::Observed)
                | (State::Requested, State::Failed)
                | (State::Requested, State::ReconciliationRequired)
        );
        valid.then_some(()).ok_or_else(|| {
            CredentialRefreshError::new(
                "credential-refresh-state-invalid",
                "credential refresh observation state moved backwards",
            )
        })
    }
}

impl<'de> Deserialize<'de> for CredentialRefreshRequest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = RequestWire::deserialize(deserializer)?;
        let request = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            provider_id: wire.provider_id,
            profile_id: wire.profile_id,
            credential_identity: wire.credential_identity,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            refresh_authority: wire.refresh_authority,
            network_authority: wire.network_authority,
            credential_value_present: wire.credential_value_present,
            credential_value_retained: wire.credential_value_retained,
        };
        request.validate().map_err(serde::de::Error::custom)?;
        Ok(request)
    }
}

impl Serialize for CredentialRefreshRequest {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            schema_version: &'a str,
            operation_identity: &'a str,
            provider_id: &'a str,
            profile_id: &'a str,
            credential_identity: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            refresh_authority: bool,
            network_authority: bool,
            credential_value_present: bool,
            credential_value_retained: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            provider_id: &self.provider_id,
            profile_id: &self.profile_id,
            credential_identity: &self.credential_identity,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            refresh_authority: false,
            network_authority: false,
            credential_value_present: false,
            credential_value_retained: false,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for CredentialRefreshObservation {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = ObservationWire::deserialize(deserializer)?;
        let observation = Self {
            schema_version: wire.schema_version,
            operation_identity: wire.operation_identity,
            provider_id: wire.provider_id,
            profile_id: wire.profile_id,
            credential_identity: wire.credential_identity,
            idempotency_key: wire.idempotency_key,
            request_fingerprint: wire.request_fingerprint,
            revision: wire.revision,
            state: wire.state,
            observation: wire.observation,
            observation_identity: wire.observation_identity,
            observed_at_ms: wire.observed_at_ms,
            refresh_authority: wire.refresh_authority,
            network_authority: wire.network_authority,
            credential_value_present: wire.credential_value_present,
            credential_value_retained: wire.credential_value_retained,
        };
        observation.validate().map_err(serde::de::Error::custom)?;
        Ok(observation)
    }
}

impl Serialize for CredentialRefreshObservation {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            schema_version: &'a str,
            operation_identity: &'a str,
            provider_id: &'a str,
            profile_id: &'a str,
            credential_identity: &'a str,
            idempotency_key: &'a str,
            request_fingerprint: &'a str,
            revision: u64,
            state: State,
            observation: Observation,
            observation_identity: Option<&'a str>,
            observed_at_ms: u64,
            refresh_authority: bool,
            network_authority: bool,
            credential_value_present: bool,
            credential_value_retained: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            operation_identity: &self.operation_identity,
            provider_id: &self.provider_id,
            profile_id: &self.profile_id,
            credential_identity: &self.credential_identity,
            idempotency_key: &self.idempotency_key,
            request_fingerprint: &self.request_fingerprint,
            revision: self.revision,
            state: self.state,
            observation: self.observation,
            observation_identity: self.observation_identity.as_deref(),
            observed_at_ms: self.observed_at_ms,
            refresh_authority: false,
            network_authority: false,
            credential_value_present: false,
            credential_value_retained: false,
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

    fn credential_identity(byte: char) -> String {
        format!("credential:sha256:{}", byte.to_string().repeat(64))
    }

    fn observation_identity(byte: char) -> String {
        format!(
            "credential-refresh-observation:sha256:{}",
            byte.to_string().repeat(64)
        )
    }

    fn request() -> CredentialRefreshRequest {
        CredentialRefreshRequest::new(
            "openai",
            "profile-main",
            credential_identity('a'),
            "refresh-1",
            fingerprint('b'),
        )
        .unwrap()
    }

    #[test]
    fn strict_metadata_round_trip_has_no_credential_or_authority() {
        let request = request();
        let encoded = serde_json::to_value(&request).unwrap();
        for field in [
            "refresh_authority",
            "network_authority",
            "credential_value_present",
            "credential_value_retained",
        ] {
            assert_eq!(encoded[field], false);
        }
        for forbidden in [
            "credential",
            "credential_value",
            "access_token",
            "refresh_token",
            "authorization",
            "provider_body",
            "headers",
        ] {
            assert!(encoded.get(forbidden).is_none());
        }
        assert_eq!(
            serde_json::from_value::<CredentialRefreshRequest>(encoded).unwrap(),
            request
        );

        let observed = request
            .observation(
                State::Observed,
                Observation::Unavailable,
                2,
                Some(observation_identity('c')),
                11,
            )
            .unwrap();
        let encoded = serde_json::to_value(&observed).unwrap();
        for forbidden in [
            "credential_value",
            "token",
            "provider_body",
            "response_body",
            "authorization",
        ] {
            assert!(encoded.get(forbidden).is_none());
        }
        assert_eq!(
            serde_json::from_value::<CredentialRefreshObservation>(encoded).unwrap(),
            observed
        );
    }

    #[test]
    fn identities_bind_all_request_metadata_and_retries_are_idempotent() {
        let original = request();
        let same = CredentialRefreshRequest::new(
            "openai",
            "profile-main",
            credential_identity('a'),
            "refresh-1",
            fingerprint('b'),
        )
        .unwrap();
        assert_eq!(original.operation_identity, same.operation_identity);
        assert!(same.validate_retry(&original).is_ok());

        for (provider, profile, credential, fingerprint_byte) in [
            ("anthropic", "profile-main", 'a', 'b'),
            ("openai", "profile-other", 'a', 'b'),
            ("openai", "profile-main", 'c', 'b'),
            ("openai", "profile-main", 'a', 'd'),
        ] {
            let different = CredentialRefreshRequest::new(
                provider,
                profile,
                credential_identity(credential),
                "refresh-1",
                fingerprint(fingerprint_byte),
            )
            .unwrap();
            assert_ne!(original.operation_identity, different.operation_identity);
            assert_eq!(
                different.validate_retry(&original).unwrap_err().code,
                "credential-refresh-idempotency-conflict"
            );
        }
    }

    #[test]
    fn lifecycle_is_contiguous_monotonic_and_reconciliation_is_frozen() {
        let request = request();
        let requested = request
            .observation(State::Requested, Observation::Unresolved, 1, None, 10)
            .unwrap();
        assert!(requested.can_follow(None).is_ok());
        assert!(requested.can_follow(Some(&requested)).is_ok());

        let unavailable = request
            .observation(
                State::Observed,
                Observation::Unavailable,
                2,
                Some(observation_identity('d')),
                11,
            )
            .unwrap();
        assert!(unavailable.can_follow(Some(&requested)).is_ok());
        assert!(unavailable.can_follow(Some(&unavailable)).is_ok());
        assert!(unavailable.matches_request(&request));

        let skipped = request
            .observation(
                State::Failed,
                Observation::Unresolved,
                4,
                Some(observation_identity('e')),
                12,
            )
            .unwrap();
        assert_eq!(
            skipped.can_follow(Some(&requested)).unwrap_err().code,
            "credential-refresh-revision-invalid"
        );

        let reversed_time = request
            .observation(
                State::Failed,
                Observation::Unresolved,
                2,
                Some(observation_identity('f')),
                9,
            )
            .unwrap();
        assert_eq!(
            reversed_time.can_follow(Some(&requested)).unwrap_err().code,
            "credential-refresh-time-invalid"
        );

        let uncertain = request
            .observation(
                State::ReconciliationRequired,
                Observation::Unresolved,
                2,
                Some(observation_identity('7')),
                11,
            )
            .unwrap();
        assert!(uncertain.can_follow(Some(&requested)).is_ok());
        assert!(uncertain.can_follow(Some(&uncertain)).is_ok());
        let after_uncertain = request
            .observation(
                State::Observed,
                Observation::Unchanged,
                3,
                Some(observation_identity('8')),
                12,
            )
            .unwrap();
        assert_eq!(
            after_uncertain
                .can_follow(Some(&uncertain))
                .unwrap_err()
                .code,
            "credential-refresh-reconciliation-required"
        );
    }

    #[test]
    fn unknown_fields_secret_shapes_and_authority_fail_closed() {
        let request = request();
        for id in [
            "api_key=example",
            "refresh_token_value",
            "sk-123456789012345678901234",
            "abcdefgh.ijklmnop.qrstuvwx",
            "Bearer abcdefghijklmnopqrstuv",
        ] {
            assert!(CredentialRefreshRequest::new(
                "openai",
                id,
                credential_identity('a'),
                "refresh-1",
                fingerprint('b'),
            )
            .is_err());
        }
        assert!(CredentialRefreshRequest::new(
            "openai",
            "profile-main",
            credential_identity('a'),
            "refresh.v1",
            fingerprint('b'),
        )
        .is_ok());

        let mut forged = serde_json::to_value(&request).unwrap();
        forged["network_authority"] = json!(true);
        assert!(serde_json::from_value::<CredentialRefreshRequest>(forged).is_err());
        let mut unknown = serde_json::to_value(&request).unwrap();
        unknown["access_token"] = json!("must not be accepted");
        assert!(serde_json::from_value::<CredentialRefreshRequest>(unknown).is_err());

        for credential in [
            "secret-value",
            "access-token-value",
            "credential-value",
            "credential:raw",
        ] {
            assert!(CredentialRefreshRequest::new(
                "openai",
                "profile-main",
                credential,
                "refresh-1",
                fingerprint('b'),
            )
            .is_err());
        }
        assert!(request
            .observation(State::Observed, Observation::Unavailable, 0, None, 10)
            .is_err());
        assert!(request
            .observation(
                State::Observed,
                Observation::Unavailable,
                2,
                Some("credential-refresh-observation:sha256:bad".into()),
                10,
            )
            .is_err());
    }
}
