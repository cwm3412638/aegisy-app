//! Bounded, metadata-only structured user-input contract.
//!
//! Questions and options are represented only by opaque identifiers and
//! content-addressed identities.  This module deliberately does not retain a
//! label, prompt, answer, option value, or form body.  It is a lifecycle
//! contract for future AAP producers, not a user-decision, permission,
//! execution, or mutation authority.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fmt;

pub const SCHEMA_VERSION: &str = "structured-user-input/0.1";
const OPTION_IDENTITY_PREFIX: &str = "user-input-option:sha256:";
const QUESTION_IDENTITY_PREFIX: &str = "user-input-question:sha256:";
const REQUEST_IDENTITY_PREFIX: &str = "user-input-request:sha256:";
const OPERATION_IDENTITY_PREFIX: &str = "user-input-operation:sha256:";
const CANCELLATION_IDENTITY_PREFIX: &str = "user-input-cancellation:sha256:";
const OBSERVATION_IDENTITY_PREFIX: &str = "user-input-observation:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_QUESTIONS: usize = 16;
const MAX_OPTIONS_PER_QUESTION: usize = 16;
const MAX_TOTAL_OPTIONS: usize = 128;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StructuredUserInputError {
    pub code: &'static str,
    pub message: &'static str,
}

impl StructuredUserInputError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for StructuredUserInputError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
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
        || value
            .split(|character: char| {
                !character.is_ascii_alphanumeric()
                    && character != '_'
                    && character != '-'
                    && character != '.'
            })
            .any(|token| {
                let jwt = token.split('.').collect::<Vec<_>>();
                (token.starts_with("sk-") && token.len() >= 20)
                    || (token.starts_with("ghp_") && token.len() >= 20)
                    || (token.starts_with("github_pat_") && token.len() >= 24)
                    || (jwt.len() == 3
                        && jwt.iter().all(|part| {
                            part.len() >= 8
                                && part.bytes().all(|byte| {
                                    byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_')
                                })
                        }))
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

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum QuestionKind {
    SingleSelect,
    MultiSelect,
    Confirm,
    Text,
}

impl QuestionKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::SingleSelect => "single-select",
            Self::MultiSelect => "multi-select",
            Self::Confirm => "confirm",
            Self::Text => "text",
        }
    }

    fn requires_options(self) -> bool {
        matches!(self, Self::SingleSelect | Self::MultiSelect)
    }
}

fn option_identity(option_id: &str) -> Result<String, StructuredUserInputError> {
    if !valid_identifier(option_id) {
        return Err(StructuredUserInputError::new(
            "user-input-option-invalid",
            "structured user-input option identifier is invalid",
        ));
    }
    Ok(hash_identity(
        OPTION_IDENTITY_PREFIX,
        b"aegisy-user-input-option/0.1",
        &[option_id],
    ))
}

fn question_identity(
    question_id: &str,
    kind: QuestionKind,
    required: bool,
    options: &[OptionSpec],
) -> Result<String, StructuredUserInputError> {
    if !valid_identifier(question_id) {
        return Err(StructuredUserInputError::new(
            "user-input-question-invalid",
            "structured user-input question identifier is invalid",
        ));
    }
    let mut values = vec![
        question_id,
        kind.as_str(),
        if required { "true" } else { "false" },
    ];
    for option in options {
        option.validate()?;
        values.push(&option.option_identity);
    }
    Ok(hash_identity(
        QUESTION_IDENTITY_PREFIX,
        b"aegisy-user-input-question/0.1",
        &values,
    ))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OptionSpec {
    pub option_id: String,
    pub option_identity: String,
}

impl OptionSpec {
    pub fn new(option_id: impl Into<String>) -> Result<Self, StructuredUserInputError> {
        let option_id = option_id.into();
        let option = Self {
            option_identity: option_identity(&option_id)?,
            option_id,
        };
        option.validate()?;
        Ok(option)
    }

    pub fn validate(&self) -> Result<(), StructuredUserInputError> {
        if !valid_identifier(&self.option_id)
            || !valid_sha256_identity(&self.option_identity, OPTION_IDENTITY_PREFIX)
            || self.option_identity != option_identity(&self.option_id)?
        {
            return Err(StructuredUserInputError::new(
                "user-input-option-invalid",
                "structured user-input option identity is invalid",
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct OptionWire {
    option_id: String,
    option_identity: String,
}

impl<'de> Deserialize<'de> for OptionSpec {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = OptionWire::deserialize(deserializer)?;
        let option = Self {
            option_id: wire.option_id,
            option_identity: wire.option_identity,
        };
        option.validate().map_err(serde::de::Error::custom)?;
        Ok(option)
    }
}

impl Serialize for OptionSpec {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            option_id: &'a str,
            option_identity: &'a str,
        }
        Wire {
            option_id: &self.option_id,
            option_identity: &self.option_identity,
        }
        .serialize(serializer)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Question {
    pub question_id: String,
    pub question_identity: String,
    pub kind: QuestionKind,
    pub required: bool,
    pub options: Vec<OptionSpec>,
}

impl Question {
    pub fn new(
        question_id: impl Into<String>,
        kind: QuestionKind,
        required: bool,
        options: Vec<OptionSpec>,
    ) -> Result<Self, StructuredUserInputError> {
        let question_id = question_id.into();
        let question_identity = question_identity(&question_id, kind, required, &options)?;
        let question = Self {
            question_id,
            question_identity,
            kind,
            required,
            options,
        };
        question.validate()?;
        Ok(question)
    }

    pub fn validate(&self) -> Result<(), StructuredUserInputError> {
        if !valid_identifier(&self.question_id)
            || !valid_sha256_identity(&self.question_identity, QUESTION_IDENTITY_PREFIX)
        {
            return Err(StructuredUserInputError::new(
                "user-input-question-invalid",
                "structured user-input question identity is invalid",
            ));
        }
        if self.options.len() > MAX_OPTIONS_PER_QUESTION
            || (self.kind.requires_options() && self.options.is_empty())
            || (!self.kind.requires_options() && !self.options.is_empty())
        {
            return Err(StructuredUserInputError::new(
                "user-input-question-bounds-invalid",
                "structured user-input question options are outside their bound",
            ));
        }
        let mut option_ids = BTreeSet::new();
        for option in &self.options {
            option.validate()?;
            if !option_ids.insert(&option.option_id) {
                return Err(StructuredUserInputError::new(
                    "user-input-option-duplicate",
                    "structured user-input question contains a duplicate option",
                ));
            }
        }
        let expected =
            question_identity(&self.question_id, self.kind, self.required, &self.options)?;
        if self.question_identity != expected {
            return Err(StructuredUserInputError::new(
                "user-input-question-invalid",
                "structured user-input question identity does not match its metadata",
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct QuestionWire {
    question_id: String,
    question_identity: String,
    kind: QuestionKind,
    required: bool,
    options: Vec<OptionSpec>,
}

impl<'de> Deserialize<'de> for Question {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = QuestionWire::deserialize(deserializer)?;
        let question = Self {
            question_id: wire.question_id,
            question_identity: wire.question_identity,
            kind: wire.kind,
            required: wire.required,
            options: wire.options,
        };
        question.validate().map_err(serde::de::Error::custom)?;
        Ok(question)
    }
}

impl Serialize for Question {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            question_id: &'a str,
            question_identity: &'a str,
            kind: QuestionKind,
            required: bool,
            options: &'a [OptionSpec],
        }
        Wire {
            question_id: &self.question_id,
            question_identity: &self.question_identity,
            kind: self.kind,
            required: self.required,
            options: &self.options,
        }
        .serialize(serializer)
    }
}

fn request_identity(
    request_id: &str,
    request_fingerprint: &str,
    questions: &[Question],
) -> Result<String, StructuredUserInputError> {
    if !valid_identifier(request_id) {
        return Err(StructuredUserInputError::new(
            "user-input-request-invalid",
            "structured user-input request identifier is invalid",
        ));
    }
    if !valid_sha256_identity(request_fingerprint, "request:sha256:") {
        return Err(StructuredUserInputError::new(
            "user-input-fingerprint-invalid",
            "structured user-input request fingerprint is invalid",
        ));
    }
    let mut values = vec![request_id, request_fingerprint];
    for question in questions {
        question.validate()?;
        values.push(&question.question_identity);
    }
    Ok(hash_identity(
        REQUEST_IDENTITY_PREFIX,
        b"aegisy-structured-user-input-request/0.1",
        &values,
    ))
}

fn operation_identity(
    session_id: &str,
    turn_id: &str,
    request_identity: &str,
    idempotency_key: &str,
    request_fingerprint: &str,
) -> Result<String, StructuredUserInputError> {
    for value in [session_id, turn_id, idempotency_key] {
        if !valid_identifier(value) {
            return Err(StructuredUserInputError::new(
                "user-input-identity-invalid",
                "structured user-input operation identity is invalid",
            ));
        }
    }
    if !valid_sha256_identity(request_identity, REQUEST_IDENTITY_PREFIX)
        || !valid_sha256_identity(request_fingerprint, "request:sha256:")
    {
        return Err(StructuredUserInputError::new(
            "user-input-binding-invalid",
            "structured user-input request binding is invalid",
        ));
    }
    Ok(hash_identity(
        OPERATION_IDENTITY_PREFIX,
        b"aegisy-structured-user-input-operation/0.1",
        &[
            session_id,
            turn_id,
            request_identity,
            idempotency_key,
            request_fingerprint,
        ],
    ))
}

fn cancellation_identity(
    operation_identity: &str,
    cancellation_idempotency_key: &str,
) -> Result<String, StructuredUserInputError> {
    if !valid_sha256_identity(operation_identity, OPERATION_IDENTITY_PREFIX)
        || !valid_identifier(cancellation_idempotency_key)
    {
        return Err(StructuredUserInputError::new(
            "user-input-cancellation-invalid",
            "structured user-input cancellation binding is invalid",
        ));
    }
    Ok(hash_identity(
        CANCELLATION_IDENTITY_PREFIX,
        b"aegisy-structured-user-input-cancellation/0.1",
        &[operation_identity, cancellation_idempotency_key],
    ))
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum State {
    Requested,
    CancellationRequested,
    Cancelled,
    Resolved,
    Failed,
    ReconciliationRequired,
}

impl State {
    fn is_terminal(self) -> bool {
        matches!(self, Self::Cancelled | Self::Resolved | Self::Failed)
    }
}

/// No variant contains or implies an answer.  `Completed` is only a
/// content-free observation that the request boundary ended normally.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Resolution {
    Unresolved,
    Completed,
    Cancelled,
    Expired,
    NotSupported,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StructuredUserInputRequest {
    pub schema_version: String,
    pub operation_identity: String,
    pub request_identity: String,
    pub session_id: String,
    pub turn_id: String,
    pub request_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub questions: Vec<Question>,
    pub user_decision_observed: bool,
    pub permission_authority: bool,
    pub execution_authority: bool,
    pub mutation_authority: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StructuredUserInputAcknowledgement {
    pub schema_version: String,
    pub operation_identity: String,
    pub request_identity: String,
    pub session_id: String,
    pub turn_id: String,
    pub request_id: String,
    pub idempotency_key: String,
    pub request_fingerprint: String,
    pub questions: Vec<Question>,
    pub revision: u64,
    pub state: State,
    pub resolution: Resolution,
    pub cancellation_idempotency_key: Option<String>,
    pub cancellation_identity: Option<String>,
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
    questions: Vec<Question>,
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
    questions: Vec<Question>,
    revision: u64,
    state: State,
    resolution: Resolution,
    cancellation_idempotency_key: Option<String>,
    cancellation_identity: Option<String>,
    observation_identity: Option<String>,
    observed_at_ms: u64,
    user_decision_observed: bool,
    permission_authority: bool,
    execution_authority: bool,
    mutation_authority: bool,
}

impl StructuredUserInputRequest {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        session_id: impl Into<String>,
        turn_id: impl Into<String>,
        request_id: impl Into<String>,
        idempotency_key: impl Into<String>,
        request_fingerprint: impl Into<String>,
        questions: Vec<Question>,
    ) -> Result<Self, StructuredUserInputError> {
        let session_id = session_id.into();
        let turn_id = turn_id.into();
        let request_id = request_id.into();
        let idempotency_key = idempotency_key.into();
        let request_fingerprint = request_fingerprint.into();
        let request_identity = request_identity(&request_id, &request_fingerprint, &questions)?;
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
            questions,
            user_decision_observed: false,
            permission_authority: false,
            execution_authority: false,
            mutation_authority: false,
        };
        request.validate()?;
        Ok(request)
    }

    pub fn validate(&self) -> Result<(), StructuredUserInputError> {
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.operation_identity, OPERATION_IDENTITY_PREFIX)
            || !valid_sha256_identity(&self.request_identity, REQUEST_IDENTITY_PREFIX)
            || self.request_identity
                != request_identity(&self.request_id, &self.request_fingerprint, &self.questions)?
            || self.operation_identity
                != operation_identity(
                    &self.session_id,
                    &self.turn_id,
                    &self.request_identity,
                    &self.idempotency_key,
                    &self.request_fingerprint,
                )?
        {
            return Err(StructuredUserInputError::new(
                "user-input-request-invalid",
                "structured user-input request schema or identity is invalid",
            ));
        }
        for value in [
            &self.session_id,
            &self.turn_id,
            &self.request_id,
            &self.idempotency_key,
        ] {
            if !valid_identifier(value) {
                return Err(StructuredUserInputError::new(
                    "user-input-request-invalid",
                    "structured user-input request identifier is invalid",
                ));
            }
        }
        if !valid_sha256_identity(&self.request_fingerprint, "request:sha256:") {
            return Err(StructuredUserInputError::new(
                "user-input-fingerprint-invalid",
                "structured user-input request fingerprint is invalid",
            ));
        }
        if self.questions.is_empty() || self.questions.len() > MAX_QUESTIONS {
            return Err(StructuredUserInputError::new(
                "user-input-question-bounds-invalid",
                "structured user-input question count is outside its bound",
            ));
        }
        let mut question_ids = BTreeSet::new();
        let mut total_options = 0;
        for question in &self.questions {
            question.validate()?;
            if !question_ids.insert(&question.question_id) {
                return Err(StructuredUserInputError::new(
                    "user-input-question-duplicate",
                    "structured user-input request contains a duplicate question",
                ));
            }
            total_options += question.options.len();
        }
        if total_options > MAX_TOTAL_OPTIONS {
            return Err(StructuredUserInputError::new(
                "user-input-option-bounds-invalid",
                "structured user-input option count is outside its bound",
            ));
        }
        if self.user_decision_observed
            || self.permission_authority
            || self.execution_authority
            || self.mutation_authority
        {
            return Err(StructuredUserInputError::new(
                "user-input-authority-invalid",
                "structured user-input metadata cannot claim an answer or grant authority",
            ));
        }
        Ok(())
    }

    pub fn validate_retry(&self, reserved: &Self) -> Result<(), StructuredUserInputError> {
        self.validate()?;
        reserved.validate()?;
        if self == reserved {
            Ok(())
        } else {
            Err(StructuredUserInputError::new(
                "user-input-idempotency-conflict",
                "structured user-input idempotency key is already bound to different metadata",
            ))
        }
    }

    pub fn acknowledgement(
        &self,
        state: State,
        resolution: Resolution,
        revision: u64,
        cancellation_idempotency_key: Option<String>,
        observation_identity: Option<String>,
        observed_at_ms: u64,
    ) -> Result<StructuredUserInputAcknowledgement, StructuredUserInputError> {
        self.validate()?;
        let cancellation_identity = cancellation_idempotency_key
            .as_deref()
            .map(|key| cancellation_identity(&self.operation_identity, key))
            .transpose()?;
        let acknowledgement = StructuredUserInputAcknowledgement {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            request_identity: self.request_identity.clone(),
            session_id: self.session_id.clone(),
            turn_id: self.turn_id.clone(),
            request_id: self.request_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            questions: self.questions.clone(),
            revision,
            state,
            resolution,
            cancellation_idempotency_key,
            cancellation_identity,
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

    pub fn cancellation(
        &self,
        cancellation_idempotency_key: impl Into<String>,
        revision: u64,
        observed_at_ms: u64,
    ) -> Result<StructuredUserInputAcknowledgement, StructuredUserInputError> {
        let key = cancellation_idempotency_key.into();
        self.acknowledgement(
            State::CancellationRequested,
            Resolution::Unresolved,
            revision,
            Some(key),
            None,
            observed_at_ms,
        )
    }

    pub fn cancellation_identity(
        &self,
        cancellation_idempotency_key: &str,
    ) -> Result<String, StructuredUserInputError> {
        self.validate()?;
        cancellation_identity(&self.operation_identity, cancellation_idempotency_key)
    }
}

impl StructuredUserInputAcknowledgement {
    pub fn validate(&self) -> Result<(), StructuredUserInputError> {
        let request = StructuredUserInputRequest {
            schema_version: SCHEMA_VERSION.into(),
            operation_identity: self.operation_identity.clone(),
            request_identity: self.request_identity.clone(),
            session_id: self.session_id.clone(),
            turn_id: self.turn_id.clone(),
            request_id: self.request_id.clone(),
            idempotency_key: self.idempotency_key.clone(),
            request_fingerprint: self.request_fingerprint.clone(),
            questions: self.questions.clone(),
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
            return Err(StructuredUserInputError::new(
                "user-input-ack-bounds-invalid",
                "structured user-input acknowledgement revision or time is outside its bound",
            ));
        }
        match (
            &self.cancellation_idempotency_key,
            &self.cancellation_identity,
        ) {
            (Some(key), Some(identity))
                if valid_identifier(key)
                    && valid_sha256_identity(identity, CANCELLATION_IDENTITY_PREFIX)
                    && cancellation_identity(&self.operation_identity, key)
                        .is_ok_and(|expected| expected == *identity) => {}
            (None, None) => {}
            _ => {
                return Err(StructuredUserInputError::new(
                    "user-input-cancellation-invalid",
                    "structured user-input cancellation binding is invalid",
                ));
            }
        }
        if let Some(identity) = &self.observation_identity {
            if !valid_sha256_identity(identity, OBSERVATION_IDENTITY_PREFIX) {
                return Err(StructuredUserInputError::new(
                    "user-input-observation-invalid",
                    "structured user-input observation identity is invalid",
                ));
            }
        }
        match self.state {
            State::Requested => {
                if self.resolution != Resolution::Unresolved
                    || self.cancellation_idempotency_key.is_some()
                    || self.cancellation_identity.is_some()
                    || self.observation_identity.is_some()
                {
                    return Err(StructuredUserInputError::new(
                        "user-input-observation-order-invalid",
                        "requested user-input metadata cannot carry an outcome",
                    ));
                }
            }
            State::CancellationRequested => {
                if self.resolution != Resolution::Unresolved
                    || self.cancellation_idempotency_key.is_none()
                    || self.cancellation_identity.is_none()
                    || self.observation_identity.is_some()
                {
                    return Err(StructuredUserInputError::new(
                        "user-input-cancellation-invalid",
                        "cancellation request requires only its bound cancellation identity",
                    ));
                }
            }
            State::Cancelled => {
                if self.resolution != Resolution::Cancelled
                    || self.cancellation_idempotency_key.is_none()
                    || self.cancellation_identity.is_none()
                    || self.observation_identity.is_none()
                {
                    return Err(StructuredUserInputError::new(
                        "user-input-cancellation-invalid",
                        "cancelled user-input metadata requires cancellation and observation identities",
                    ));
                }
            }
            State::Resolved => {
                if matches!(
                    self.resolution,
                    Resolution::Unresolved | Resolution::Cancelled
                ) || self.observation_identity.is_none()
                {
                    return Err(StructuredUserInputError::new(
                        "user-input-resolution-invalid",
                        "resolved user-input metadata requires a content-free observation",
                    ));
                }
            }
            State::Failed | State::ReconciliationRequired => {
                if self.resolution != Resolution::Unresolved || self.observation_identity.is_none()
                {
                    return Err(StructuredUserInputError::new(
                        "user-input-observation-invalid",
                        "failed or uncertain user-input metadata requires evidence without an answer",
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn matches_request(&self, request: &StructuredUserInputRequest) -> bool {
        self.validate().is_ok()
            && request.validate().is_ok()
            && self.operation_identity == request.operation_identity
            && self.request_identity == request.request_identity
            && self.session_id == request.session_id
            && self.turn_id == request.turn_id
            && self.request_id == request.request_id
            && self.idempotency_key == request.idempotency_key
            && self.request_fingerprint == request.request_fingerprint
            && self.questions == request.questions
    }

    pub fn can_follow(&self, previous: Option<&Self>) -> Result<(), StructuredUserInputError> {
        self.validate()?;
        let Some(previous) = previous else {
            return (self.state == State::Requested && self.revision == 1)
                .then_some(())
                .ok_or_else(|| {
                    StructuredUserInputError::new(
                        "user-input-state-invalid",
                        "structured user-input acknowledgement must begin at requested revision one",
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
            || self.questions != previous.questions
        {
            return Err(StructuredUserInputError::new(
                "user-input-binding-changed",
                "structured user-input acknowledgement binding changed",
            ));
        }
        if self == previous {
            return Ok(());
        }
        if self.revision == previous.revision {
            return Err(StructuredUserInputError::new(
                "user-input-idempotency-conflict",
                "structured user-input acknowledgement revision is already bound to different metadata",
            ));
        }
        if self.revision != previous.revision + 1 {
            return Err(StructuredUserInputError::new(
                "user-input-revision-invalid",
                "structured user-input acknowledgement revision is not contiguous",
            ));
        }
        if self.observed_at_ms < previous.observed_at_ms {
            return Err(StructuredUserInputError::new(
                "user-input-time-invalid",
                "structured user-input acknowledgement time moved backwards",
            ));
        }
        if previous.state == State::ReconciliationRequired {
            return Err(StructuredUserInputError::new(
                "user-input-reconciliation-required",
                "uncertain structured user-input metadata cannot be advanced by this producer",
            ));
        }
        if previous.state == State::CancellationRequested
            && (self.cancellation_idempotency_key != previous.cancellation_idempotency_key
                || self.cancellation_identity != previous.cancellation_identity)
        {
            return Err(StructuredUserInputError::new(
                "user-input-cancellation-binding-changed",
                "structured user-input cancellation identity changed",
            ));
        }
        if previous.state.is_terminal() {
            return Err(StructuredUserInputError::new(
                "user-input-state-invalid",
                "terminal structured user-input metadata cannot be advanced",
            ));
        }
        let valid = matches!(
            (previous.state, self.state),
            (State::Requested, State::CancellationRequested)
                | (State::Requested, State::Resolved)
                | (State::Requested, State::Failed)
                | (State::Requested, State::ReconciliationRequired)
                | (State::CancellationRequested, State::Cancelled)
                | (State::CancellationRequested, State::Resolved)
                | (State::CancellationRequested, State::Failed)
                | (State::CancellationRequested, State::ReconciliationRequired)
        );
        valid.then_some(()).ok_or_else(|| {
            StructuredUserInputError::new(
                "user-input-state-invalid",
                "structured user-input acknowledgement state moved backwards",
            )
        })
    }
}

impl<'de> Deserialize<'de> for StructuredUserInputRequest {
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
            questions: wire.questions,
            user_decision_observed: wire.user_decision_observed,
            permission_authority: wire.permission_authority,
            execution_authority: wire.execution_authority,
            mutation_authority: wire.mutation_authority,
        };
        request.validate().map_err(serde::de::Error::custom)?;
        Ok(request)
    }
}

impl Serialize for StructuredUserInputRequest {
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
            questions: &'a [Question],
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
            questions: &self.questions,
            user_decision_observed: false,
            permission_authority: false,
            execution_authority: false,
            mutation_authority: false,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for StructuredUserInputAcknowledgement {
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
            questions: wire.questions,
            revision: wire.revision,
            state: wire.state,
            resolution: wire.resolution,
            cancellation_idempotency_key: wire.cancellation_idempotency_key,
            cancellation_identity: wire.cancellation_identity,
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

impl Serialize for StructuredUserInputAcknowledgement {
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
            questions: &'a [Question],
            revision: u64,
            state: State,
            resolution: Resolution,
            cancellation_idempotency_key: Option<&'a str>,
            cancellation_identity: Option<&'a str>,
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
            questions: &self.questions,
            revision: self.revision,
            state: self.state,
            resolution: self.resolution,
            cancellation_idempotency_key: self.cancellation_idempotency_key.as_deref(),
            cancellation_identity: self.cancellation_identity.as_deref(),
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

    fn request() -> StructuredUserInputRequest {
        let options = vec![
            OptionSpec::new("yes").unwrap(),
            OptionSpec::new("no").unwrap(),
        ];
        let question = Question::new("confirm", QuestionKind::SingleSelect, true, options).unwrap();
        StructuredUserInputRequest::new(
            "session-1",
            "turn-1",
            "question-request-1",
            "retry-1",
            fingerprint('1'),
            vec![question],
        )
        .unwrap()
    }

    #[test]
    fn question_and_option_identities_are_strict_and_content_free() {
        let request = request();
        let encoded = serde_json::to_value(&request).unwrap();
        assert!(encoded.get("prompt").is_none());
        assert!(encoded.get("answer").is_none());
        assert!(encoded["questions"][0].get("label").is_none());
        assert!(encoded["questions"][0]["options"][0].get("value").is_none());
        let decoded: StructuredUserInputRequest = serde_json::from_value(encoded).unwrap();
        assert_eq!(decoded, request);
        assert_ne!(
            request.questions[0].question_identity,
            request.questions[0].options[0].option_identity
        );
    }

    #[test]
    fn bounds_and_duplicate_questions_or_options_fail_closed() {
        assert!(Question::new("text", QuestionKind::Text, false, vec![]).is_ok());
        assert!(Question::new("bad", QuestionKind::SingleSelect, true, vec![]).is_err());
        let duplicate_options = vec![
            OptionSpec::new("same").unwrap(),
            OptionSpec::new("same").unwrap(),
        ];
        assert!(Question::new(
            "choose",
            QuestionKind::SingleSelect,
            true,
            duplicate_options
        )
        .is_err());
        let q = Question::new(
            "choose",
            QuestionKind::SingleSelect,
            true,
            vec![OptionSpec::new("one").unwrap()],
        )
        .unwrap();
        assert!(StructuredUserInputRequest::new(
            "session-1",
            "turn-1",
            "request-1",
            "retry-1",
            fingerprint('1'),
            vec![q.clone(), q],
        )
        .is_err());
        let too_many = (0..MAX_QUESTIONS + 1)
            .map(|index| {
                Question::new(format!("q-{index}"), QuestionKind::Text, false, vec![]).unwrap()
            })
            .collect();
        assert!(StructuredUserInputRequest::new(
            "session-1",
            "turn-1",
            "request-1",
            "retry-1",
            fingerprint('1'),
            too_many,
        )
        .is_err());
    }

    #[test]
    fn request_retry_and_question_identity_drift_are_idempotent_or_conflicting() {
        let original = request();
        let equivalent = request();
        assert_eq!(original, equivalent);
        assert!(original.validate_retry(&equivalent).is_ok());
        let changed_question = Question::new(
            "confirm",
            QuestionKind::SingleSelect,
            true,
            vec![
                OptionSpec::new("yes").unwrap(),
                OptionSpec::new("maybe").unwrap(),
            ],
        )
        .unwrap();
        let changed = StructuredUserInputRequest::new(
            "session-1",
            "turn-1",
            "question-request-1",
            "retry-1",
            fingerprint('1'),
            vec![changed_question],
        )
        .unwrap();
        assert_ne!(original.request_identity, changed.request_identity);
        assert!(original.validate_retry(&changed).is_err());
    }

    #[test]
    fn cancellation_is_bound_and_completion_races_are_explicit() {
        let request = request();
        let requested = request
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, None, 10)
            .unwrap();
        let cancellation = request.cancellation("cancel-1", 2, 11).unwrap();
        let repeated = request.cancellation("cancel-1", 2, 11).unwrap();
        assert_eq!(cancellation, repeated);
        assert!(cancellation.can_follow(Some(&requested)).is_ok());
        let cancelled = request
            .acknowledgement(
                State::Cancelled,
                Resolution::Cancelled,
                3,
                Some("cancel-1".into()),
                Some(observation_identity()),
                12,
            )
            .unwrap();
        assert!(cancelled.can_follow(Some(&cancellation)).is_ok());
        let different_cancel = request
            .acknowledgement(
                State::Cancelled,
                Resolution::Cancelled,
                3,
                Some("cancel-2".into()),
                Some(observation_identity()),
                12,
            )
            .unwrap();
        assert!(different_cancel.can_follow(Some(&cancellation)).is_err());
        let completed = request
            .acknowledgement(
                State::Resolved,
                Resolution::Completed,
                3,
                Some("cancel-1".into()),
                Some(observation_identity()),
                12,
            )
            .unwrap();
        assert!(completed.can_follow(Some(&cancellation)).is_ok());
        assert!(completed.matches_request(&request));

        let mut tampered = serde_json::to_value(cancellation).unwrap();
        tampered["cancellation_identity"] =
            json!(request.cancellation_identity("cancel-2").unwrap());
        assert!(serde_json::from_value::<StructuredUserInputAcknowledgement>(tampered).is_err());

        assert!(request
            .acknowledgement(
                State::Resolved,
                Resolution::Cancelled,
                2,
                None,
                Some(observation_identity()),
                11,
            )
            .is_err());
    }

    #[test]
    fn uncertainty_terminal_and_authority_rules_fail_closed() {
        let base = request();
        let requested = base
            .acknowledgement(State::Requested, Resolution::Unresolved, 1, None, None, 10)
            .unwrap();
        let uncertain = base
            .acknowledgement(
                State::ReconciliationRequired,
                Resolution::Unresolved,
                2,
                None,
                Some(observation_identity()),
                11,
            )
            .unwrap();
        assert!(uncertain.can_follow(Some(&requested)).is_ok());
        let after = base
            .acknowledgement(
                State::Resolved,
                Resolution::Completed,
                3,
                None,
                Some(observation_identity()),
                12,
            )
            .unwrap();
        assert!(after.can_follow(Some(&uncertain)).is_err());

        let mut encoded = serde_json::to_value(base).unwrap();
        encoded["permission_authority"] = json!(true);
        assert!(serde_json::from_value::<StructuredUserInputRequest>(encoded).is_err());
        let mut encoded = serde_json::to_value(request()).unwrap();
        encoded["questions"][0]["prompt"] = json!("secret answer");
        assert!(serde_json::from_value::<StructuredUserInputRequest>(encoded).is_err());
        assert!(StructuredUserInputRequest::new(
            "session-1",
            "turn-1",
            "request-1",
            "api_key-retry",
            fingerprint('1'),
            vec![Question::new("text", QuestionKind::Text, false, vec![]).unwrap()],
        )
        .is_err());
    }
}
