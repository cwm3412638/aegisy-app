//! Authenticated model-catalog refresh transport contract.
//!
//! The desktop host owns login credentials and the HTTPS request. This module
//! validates only a trusted host observation of that request: conditional
//! validators, HTTP result shape, response bounds, and the signed bundle wire
//! format. It never accepts or serializes an authorization value, performs a
//! network request, installs trust state, mutates the catalog cache, selects a
//! model, issues a token, or starts a turn.

use crate::model_catalog::CatalogState;
use crate::model_catalog_signature::{
    SignedCatalogKeyRing, SignedModelCatalog, KEY_RING_SIGNATURE_SCHEMA_VERSION,
    SIGNATURE_SCHEMA_VERSION,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const SCHEMA_VERSION: &str = "model-catalog-refresh/0.1";
pub const BUNDLE_SCHEMA_VERSION: &str = "model-catalog-refresh-bundle/0.1";
pub const STATUS_SCHEMA_VERSION: &str = "model-catalog-refresh-status/0.1";
const ENDPOINT_SCOPE: &str = "aegisy-model-catalog";
const MAX_BODY_BYTES: usize = 2 * 1024 * 1024;
const MAX_VALIDATOR_BYTES: usize = 512;
const MAX_REQUEST_ID_BYTES: usize = 128;

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct CatalogRefreshValidators {
    pub etag: Option<String>,
    pub last_modified: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct CatalogRefreshRequest {
    pub schema_version: String,
    pub request_id: String,
    pub endpoint_scope: String,
    pub started_at_ms: u64,
    pub validators: CatalogRefreshValidators,
    pub authenticated_transport_required: bool,
    pub accept_encoding: String,
    pub credentials_included: bool,
    pub body_included: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct CatalogRefreshBundle {
    pub schema_version: String,
    pub sequence: u64,
    pub signed_key_ring: Option<SignedCatalogKeyRing>,
    pub signed_catalog: SignedModelCatalog,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum CatalogRefreshOutcomeKind {
    Modified,
    NotModified,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CatalogRefreshOutcome {
    Modified {
        bundle: Box<CatalogRefreshBundle>,
        validators: CatalogRefreshValidators,
    },
    NotModified {
        validators: CatalogRefreshValidators,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogTransportResponse {
    pub authenticated_transport: bool,
    pub http_status: u16,
    pub content_type: Option<String>,
    pub content_encoding: Option<String>,
    pub validators: CatalogRefreshValidators,
    pub body: Vec<u8>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct CatalogRefreshStatus {
    pub schema_version: String,
    pub state: String,
    pub endpoint_configured: bool,
    pub trust_anchor_configured: bool,
    pub authenticated_transport_required: bool,
    pub conditional_requests_supported: bool,
    pub last_attempt_at_ms: Option<u64>,
    pub last_http_status: Option<u16>,
    pub last_outcome: Option<CatalogRefreshOutcomeKind>,
    pub last_error_code: Option<String>,
    pub retryable: bool,
    pub etag_present: bool,
    pub last_modified_present: bool,
    pub validator_identity: Option<String>,
    pub response_body_retained: bool,
    pub credentials_included: bool,
    pub cache_install_authority: bool,
    pub selection_allowed: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogRefreshError {
    pub code: &'static str,
    pub message: &'static str,
    pub retryable: bool,
    pub http_status: Option<u16>,
}

impl CatalogRefreshValidators {
    pub fn new(
        etag: Option<String>,
        last_modified: Option<String>,
    ) -> Result<Self, CatalogRefreshError> {
        let validators = Self {
            etag,
            last_modified,
        };
        validators.validate()?;
        Ok(validators)
    }

    pub fn is_empty(&self) -> bool {
        self.etag.is_none() && self.last_modified.is_none()
    }

    pub fn identity(&self) -> Result<Option<String>, CatalogRefreshError> {
        self.validate()?;
        if self.is_empty() {
            return Ok(None);
        }
        let bytes = serde_json::to_vec(self).map_err(|_| {
            error(
                "model-catalog-refresh-validator-serialize",
                "catalog refresh validators could not be serialized",
                false,
                None,
            )
        })?;
        Ok(Some(format!(
            "model-catalog-refresh-validator:sha256:{:x}",
            Sha256::digest(bytes)
        )))
    }

    fn validate(&self) -> Result<(), CatalogRefreshError> {
        if let Some(etag) = &self.etag {
            validate_header_value(etag, "model-catalog-refresh-etag-invalid")?;
            if !valid_etag(etag) {
                return Err(error(
                    "model-catalog-refresh-etag-invalid",
                    "catalog refresh ETag is invalid",
                    false,
                    None,
                ));
            }
        }
        if let Some(last_modified) = &self.last_modified {
            validate_header_value(last_modified, "model-catalog-refresh-last-modified-invalid")?;
        }
        Ok(())
    }

    fn merged_with(&self, response: &Self) -> Self {
        Self {
            etag: response.etag.clone().or_else(|| self.etag.clone()),
            last_modified: response
                .last_modified
                .clone()
                .or_else(|| self.last_modified.clone()),
        }
    }
}

impl CatalogRefreshRequest {
    pub fn new(
        request_id: impl Into<String>,
        started_at_ms: u64,
        validators: CatalogRefreshValidators,
    ) -> Result<Self, CatalogRefreshError> {
        let request = Self {
            schema_version: SCHEMA_VERSION.into(),
            request_id: request_id.into(),
            endpoint_scope: ENDPOINT_SCOPE.into(),
            started_at_ms,
            validators,
            authenticated_transport_required: true,
            accept_encoding: "identity".into(),
            credentials_included: false,
            body_included: false,
        };
        request.validate()?;
        Ok(request)
    }

    pub fn validate(&self) -> Result<(), CatalogRefreshError> {
        if self.schema_version != SCHEMA_VERSION
            || self.endpoint_scope != ENDPOINT_SCOPE
            || !self.authenticated_transport_required
            || self.accept_encoding != "identity"
            || self.credentials_included
            || self.body_included
        {
            return Err(error(
                "model-catalog-refresh-request-invalid",
                "catalog refresh request contract is invalid",
                false,
                None,
            ));
        }
        validate_identifier(&self.request_id)?;
        self.validators.validate()
    }
}

impl CatalogRefreshBundle {
    pub fn validate(&self) -> Result<(), CatalogRefreshError> {
        if self.schema_version != BUNDLE_SCHEMA_VERSION || self.sequence == 0 {
            return Err(error(
                "model-catalog-refresh-bundle-invalid",
                "catalog refresh bundle is invalid",
                false,
                Some(200),
            ));
        }
        if self.signed_catalog.schema_version != SIGNATURE_SCHEMA_VERSION
            || self.signed_catalog.catalog.state != CatalogState::Fresh
            || self.signed_catalog.catalog.signature_validated
        {
            return Err(error(
                "model-catalog-refresh-catalog-envelope-invalid",
                "catalog refresh signed catalog envelope is invalid",
                false,
                Some(200),
            ));
        }
        if let Some(signed_key_ring) = &self.signed_key_ring {
            if signed_key_ring.schema_version != KEY_RING_SIGNATURE_SCHEMA_VERSION
                || signed_key_ring.signed_at_ms == 0
                || signed_key_ring.key_ring.validate().is_err()
            {
                return Err(error(
                    "model-catalog-refresh-key-ring-envelope-invalid",
                    "catalog refresh signed key ring envelope is invalid",
                    false,
                    Some(200),
                ));
            }
        }
        Ok(())
    }
}

impl CatalogRefreshStatus {
    pub fn unconfigured() -> Self {
        Self {
            schema_version: STATUS_SCHEMA_VERSION.into(),
            state: "unconfigured".into(),
            endpoint_configured: false,
            trust_anchor_configured: false,
            authenticated_transport_required: true,
            conditional_requests_supported: true,
            last_attempt_at_ms: None,
            last_http_status: None,
            last_outcome: None,
            last_error_code: Some(
                "production-catalog-endpoint-and-trust-anchor-unavailable".into(),
            ),
            retryable: false,
            etag_present: false,
            last_modified_present: false,
            validator_identity: None,
            response_body_retained: false,
            credentials_included: false,
            cache_install_authority: false,
            selection_allowed: false,
        }
    }

    pub fn validate(&self) -> Result<(), CatalogRefreshError> {
        if self.schema_version != STATUS_SCHEMA_VERSION
            || !self.authenticated_transport_required
            || !self.conditional_requests_supported
            || self.response_body_retained
            || self.credentials_included
            || self.cache_install_authority
            || self.selection_allowed
        {
            return Err(error(
                "model-catalog-refresh-status-invalid",
                "catalog refresh status is invalid",
                false,
                None,
            ));
        }
        if self.state == "unconfigured"
            && (self.endpoint_configured
                || self.trust_anchor_configured
                || self.last_attempt_at_ms.is_some()
                || self.last_http_status.is_some()
                || self.last_outcome.is_some()
                || self.retryable
                || self.etag_present
                || self.last_modified_present
                || self.validator_identity.is_some())
        {
            return Err(error(
                "model-catalog-refresh-status-contradictory",
                "catalog refresh status is contradictory",
                false,
                None,
            ));
        }
        Ok(())
    }
}

pub fn evaluate_transport_response(
    request: &CatalogRefreshRequest,
    response: CatalogTransportResponse,
) -> Result<CatalogRefreshOutcome, CatalogRefreshError> {
    request.validate()?;
    response.validators.validate()?;
    if response.body.len() > MAX_BODY_BYTES {
        return Err(error(
            "model-catalog-refresh-response-too-large",
            "catalog refresh response exceeds its size limit",
            false,
            Some(response.http_status),
        ));
    }
    if !response.authenticated_transport {
        return Err(error(
            "model-catalog-refresh-transport-unauthenticated",
            "catalog refresh transport was not authenticated",
            false,
            Some(response.http_status),
        ));
    }

    match response.http_status {
        200 => modified_response(response),
        304 => {
            if request.validators.is_empty() || !response.body.is_empty() {
                return Err(error(
                    "model-catalog-refresh-not-modified-invalid",
                    "catalog refresh not-modified response is invalid",
                    false,
                    Some(304),
                ));
            }
            let validators = request.validators.merged_with(&response.validators);
            validators.validate()?;
            Ok(CatalogRefreshOutcome::NotModified { validators })
        }
        401 | 403 => Err(error(
            "model-catalog-refresh-authentication-required",
            "catalog refresh authentication is required",
            false,
            Some(response.http_status),
        )),
        429 => Err(error(
            "model-catalog-refresh-rate-limited",
            "catalog refresh was rate limited",
            true,
            Some(429),
        )),
        500..=599 => Err(error(
            "model-catalog-refresh-server-unavailable",
            "catalog refresh service is unavailable",
            true,
            Some(response.http_status),
        )),
        300..=399 => Err(error(
            "model-catalog-refresh-redirect-denied",
            "catalog refresh redirect is denied",
            false,
            Some(response.http_status),
        )),
        _ => Err(error(
            "model-catalog-refresh-http-status-invalid",
            "catalog refresh returned an unsupported HTTP status",
            false,
            Some(response.http_status),
        )),
    }
}

fn modified_response(
    response: CatalogTransportResponse,
) -> Result<CatalogRefreshOutcome, CatalogRefreshError> {
    if response.validators.is_empty() {
        return Err(error(
            "model-catalog-refresh-validator-missing",
            "catalog refresh response has no conditional validator",
            false,
            Some(200),
        ));
    }
    if response
        .content_encoding
        .as_deref()
        .is_some_and(|encoding| !encoding.eq_ignore_ascii_case("identity"))
    {
        return Err(error(
            "model-catalog-refresh-content-encoding-unsupported",
            "catalog refresh content encoding is unsupported",
            false,
            Some(200),
        ));
    }
    let content_type = response
        .content_type
        .as_deref()
        .and_then(|value| value.split(';').next())
        .map(str::trim);
    if content_type != Some("application/json") {
        return Err(error(
            "model-catalog-refresh-content-type-invalid",
            "catalog refresh content type is invalid",
            false,
            Some(200),
        ));
    }
    if response.body.is_empty() {
        return Err(error(
            "model-catalog-refresh-body-missing",
            "catalog refresh response body is missing",
            false,
            Some(200),
        ));
    }
    let bundle: CatalogRefreshBundle = serde_json::from_slice(&response.body).map_err(|_| {
        error(
            "model-catalog-refresh-body-invalid",
            "catalog refresh response body is invalid",
            false,
            Some(200),
        )
    })?;
    bundle.validate()?;
    Ok(CatalogRefreshOutcome::Modified {
        bundle: Box::new(bundle),
        validators: response.validators,
    })
}

fn validate_identifier(value: &str) -> Result<(), CatalogRefreshError> {
    if value.is_empty()
        || value.len() > MAX_REQUEST_ID_BYTES
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(error(
            "model-catalog-refresh-request-id-invalid",
            "catalog refresh request ID is invalid",
            false,
            None,
        ));
    }
    Ok(())
}

fn validate_header_value(value: &str, code: &'static str) -> Result<(), CatalogRefreshError> {
    if value.is_empty()
        || value.len() > MAX_VALIDATOR_BYTES
        || !value.bytes().all(|byte| matches!(byte, 0x20..=0x7e))
    {
        return Err(error(
            code,
            "catalog refresh validator is invalid",
            false,
            None,
        ));
    }
    Ok(())
}

fn valid_etag(value: &str) -> bool {
    let value = value.strip_prefix("W/").unwrap_or(value);
    value.len() >= 2 && value.starts_with('"') && value.ends_with('"')
}

fn error(
    code: &'static str,
    message: &'static str,
    retryable: bool,
    http_status: Option<u16>,
) -> CatalogRefreshError {
    CatalogRefreshError {
        code,
        message,
        retryable,
        http_status,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model_catalog::{self, ModelCatalog};
    use crate::model_catalog_signature::SIGNATURE_SCHEMA_VERSION;
    use serde::Deserialize;

    #[derive(Deserialize)]
    struct NotModifiedFixture {
        request: CatalogRefreshRequest,
        response: FixtureResponse,
    }

    #[derive(Deserialize)]
    struct FixtureResponse {
        authenticated_transport: bool,
        http_status: u16,
        validators: CatalogRefreshValidators,
    }

    fn request(validators: CatalogRefreshValidators) -> CatalogRefreshRequest {
        CatalogRefreshRequest::new("refresh-1", 1_000, validators).unwrap()
    }

    fn bundle() -> CatalogRefreshBundle {
        let mut catalog = model_catalog::offline_for_runtime(
            "codex-app-server",
            "0.144.5",
            Some("aegisy"),
            Some("fixture-model"),
        );
        catalog.catalog_version = "fixture-1".into();
        catalog.source = "aegisy-cloud".into();
        catalog.state = CatalogState::Fresh;
        catalog.signature_validated = false;
        catalog.issued_at_ms = Some(1_000);
        catalog.expires_at_ms = Some(2_000);
        CatalogRefreshBundle {
            schema_version: BUNDLE_SCHEMA_VERSION.into(),
            sequence: 1,
            signed_key_ring: None,
            signed_catalog: SignedModelCatalog {
                schema_version: SIGNATURE_SCHEMA_VERSION.into(),
                key_id: "catalog-key-1".into(),
                catalog,
                payload_identity: format!("model-catalog-signature:sha256:{}", "0".repeat(64)),
                signature_base64: "AA==".into(),
            },
        }
    }

    fn response(status: u16, body: Vec<u8>) -> CatalogTransportResponse {
        CatalogTransportResponse {
            authenticated_transport: true,
            http_status: status,
            content_type: Some("application/json; charset=utf-8".into()),
            content_encoding: Some("identity".into()),
            validators: CatalogRefreshValidators::new(Some("\"catalog-1\"".into()), None).unwrap(),
            body,
        }
    }

    #[test]
    fn deterministic_not_modified_fixture_preserves_conditional_validator() {
        let fixture: NotModifiedFixture = serde_json::from_str(include_str!(
            "../../../aap-schema/fixtures/model-catalog-refresh-not-modified.json"
        ))
        .unwrap();
        let outcome = evaluate_transport_response(
            &fixture.request,
            CatalogTransportResponse {
                authenticated_transport: fixture.response.authenticated_transport,
                http_status: fixture.response.http_status,
                content_type: None,
                content_encoding: None,
                validators: fixture.response.validators,
                body: Vec::new(),
            },
        )
        .unwrap();
        let CatalogRefreshOutcome::NotModified { validators } = outcome else {
            panic!("fixture should be not modified");
        };
        assert_eq!(validators.etag.as_deref(), Some("\"catalog-1\""));
        assert!(validators.identity().unwrap().is_some());
    }

    #[test]
    fn modified_response_returns_only_a_structurally_valid_signed_bundle() {
        let bundle = bundle();
        let body = serde_json::to_vec(&bundle).unwrap();
        let outcome = evaluate_transport_response(
            &request(CatalogRefreshValidators::default()),
            response(200, body),
        )
        .unwrap();
        let CatalogRefreshOutcome::Modified {
            bundle: parsed,
            validators,
        } = outcome
        else {
            panic!("fixture should be modified");
        };
        assert_eq!(parsed.sequence, 1);
        assert_eq!(validators.etag.as_deref(), Some("\"catalog-1\""));
    }

    #[test]
    fn unauthenticated_transport_fails_before_body_parsing() {
        let mut transport = response(200, b"not-json".to_vec());
        transport.authenticated_transport = false;
        let failure =
            evaluate_transport_response(&request(CatalogRefreshValidators::default()), transport)
                .unwrap_err();
        assert_eq!(
            failure.code,
            "model-catalog-refresh-transport-unauthenticated"
        );
        assert!(!failure.message.contains("not-json"));
        assert!(!failure.retryable);
    }

    #[test]
    fn conditional_and_response_contracts_fail_closed() {
        let no_validator = CatalogTransportResponse {
            validators: CatalogRefreshValidators::default(),
            ..response(200, serde_json::to_vec(&bundle()).unwrap())
        };
        assert_eq!(
            evaluate_transport_response(
                &request(CatalogRefreshValidators::default()),
                no_validator,
            )
            .unwrap_err()
            .code,
            "model-catalog-refresh-validator-missing"
        );

        let compressed = CatalogTransportResponse {
            content_encoding: Some("gzip".into()),
            ..response(200, serde_json::to_vec(&bundle()).unwrap())
        };
        assert_eq!(
            evaluate_transport_response(&request(CatalogRefreshValidators::default()), compressed,)
                .unwrap_err()
                .code,
            "model-catalog-refresh-content-encoding-unsupported"
        );

        let not_modified_without_request_validator = response(304, Vec::new());
        assert_eq!(
            evaluate_transport_response(
                &request(CatalogRefreshValidators::default()),
                not_modified_without_request_validator,
            )
            .unwrap_err()
            .code,
            "model-catalog-refresh-not-modified-invalid"
        );
    }

    #[test]
    fn http_failures_are_content_free_and_stably_classified() {
        for (status, code, retryable) in [
            (401, "model-catalog-refresh-authentication-required", false),
            (302, "model-catalog-refresh-redirect-denied", false),
            (429, "model-catalog-refresh-rate-limited", true),
            (503, "model-catalog-refresh-server-unavailable", true),
        ] {
            let failure = evaluate_transport_response(
                &request(CatalogRefreshValidators::default()),
                response(status, b"credential=should-not-escape".to_vec()),
            )
            .unwrap_err();
            assert_eq!(failure.code, code);
            assert_eq!(failure.retryable, retryable);
            assert_eq!(failure.http_status, Some(status));
            assert!(!failure.message.contains("credential"));
        }
    }

    #[test]
    fn status_is_content_free_and_grants_no_authority() {
        let status = CatalogRefreshStatus::unconfigured();
        status.validate().unwrap();
        let encoded = serde_json::to_string(&status).unwrap();
        assert!(encoded.contains("production-catalog-endpoint-and-trust-anchor-unavailable"));
        for forbidden in ["Authorization", "Bearer ", "access_token", "refresh_token"] {
            assert!(!encoded.contains(forbidden));
        }
        assert!(!status.cache_install_authority);
        assert!(!status.selection_allowed);
        assert!(!status.response_body_retained);
    }

    #[test]
    fn validators_reject_header_injection_and_unbounded_values() {
        assert_eq!(
            CatalogRefreshValidators::new(Some("\"ok\"\r\nInjected: value".into()), None)
                .unwrap_err()
                .code,
            "model-catalog-refresh-etag-invalid"
        );
        assert_eq!(
            CatalogRefreshValidators::new(Some(format!("\"{}\"", "x".repeat(513))), None)
                .unwrap_err()
                .code,
            "model-catalog-refresh-etag-invalid"
        );
    }

    #[test]
    fn body_size_is_bounded_before_transport_error_bodies_are_classified() {
        let failure = evaluate_transport_response(
            &request(CatalogRefreshValidators::default()),
            response(503, vec![b'x'; MAX_BODY_BYTES + 1]),
        )
        .unwrap_err();
        assert_eq!(failure.code, "model-catalog-refresh-response-too-large");
        assert!(!failure.retryable);
    }

    #[test]
    fn bundle_requires_fresh_unvalidated_catalog_envelope() {
        let mut invalid = bundle();
        invalid.signed_catalog.catalog = ModelCatalog {
            state: CatalogState::Offline,
            ..invalid.signed_catalog.catalog
        };
        assert_eq!(
            invalid.validate().unwrap_err().code,
            "model-catalog-refresh-catalog-envelope-invalid"
        );
    }
}
